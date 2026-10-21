(() => {
  'use strict';
  const $ = (selector) => document.querySelector(selector);
  const state = { page: 'tasks', tasks: [], task: null, spec: null, selection: null,
    hostDetails: null, launchMode: null,
    expanded: null, dirty: false, runtime: null, socket: null,
    terminalNode: null, terminalDecoder: null, recipe: null, selectedRun: null, experiment: null,
    terminalText: '', terminalMode: 'text', terminalCR: false,
    searchMatches: [], searchIndex: 0 };
  const esc = (value) => String(value ?? '').replace(/[&<>"']/g,
    (char) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
  let toastTimer, createStatusRequest = 0, topologyStatusRequest = 0;

  async function api(path, options = {}) {
    const response = await fetch(path, { cache: 'no-store', ...options,
      headers: options.body ? { 'Content-Type': 'application/json' } : undefined });
    let payload;
    try { payload = await response.json(); } catch { payload = {}; }
    if (!response.ok) throw new Error(payload.error || payload.output || `请求失败 (${response.status})`);
    return payload;
  }
  function toast(message) {
    const el = $('#toast');
    el.textContent = message; el.hidden = false;
    clearTimeout(toastTimer); toastTimer = setTimeout(() => { el.hidden = true; }, 4700);
  }
  function showPage(page) {
    state.page = page;
    document.querySelectorAll('[data-page]').forEach((button) => {
      if (button.dataset.page === page) button.setAttribute('aria-current', 'page');
      else button.removeAttribute('aria-current');
    });
    for (const name of ['tasks', 'topology', 'analysis']) $(`#page-${name}`).hidden = name !== page;
    if (page === 'analysis') { refreshRuntime(); refreshExperiments(); }
  }
  function taskRow(task) {
    const expanded = task.id === state.expanded;
    return `<div class="task-row ${expanded ? 'expanded' : ''}">
      <button class="task-pick" data-expand="${esc(task.id)}" aria-expanded="${expanded}">
        <span><span class="task-name">${esc(task.name)}</span><span class="task-meta">${esc(task.simulator)} · ${task.node_count} 主机 · ${task.layout === 'switch' ? '交换芯片' : 'UB 直连'}</span></span>
        <span class="task-kind ${task.editable ? 'mine' : ''}">${task.editable ? '我的任务' : '模板'}</span><span class="task-arrow">›</span></button>
      ${expanded ? `<div class="task-detail"><div class="task-detail-line"><span>仿真器 <strong>${esc(task.simulator)}</strong></span><span>拓扑 <strong>${task.layout === 'switch' ? '单交换芯片' : '直连'}</strong></span><span>主机 <strong>${task.node_count}</strong></span></div><div class="task-actions"><button class="button" data-open="${esc(task.id)}">查看拓扑</button><button class="button" data-copy="${esc(task.id)}">复制任务</button><button class="button" data-results="${esc(task.id)}">运行与日志</button><button class="button primary" data-start="${esc(task.id)}">启动仿真</button></div></div>` : ''}
    </div>`;
  }
  async function refreshTasks() {
    const data = await api('/api/tasks');
    state.tasks = data.tasks;
    $('#task-count').textContent = `${data.tasks.filter((task) => task.editable).length} 个自建任务 · ${data.tasks.filter((task) => !task.editable).length} 个模板`;
    $('#task-list').innerHTML = data.tasks.map(taskRow).join('') || '<div class="empty">暂无任务。</div>';
  }
  async function openTask(id, page = 'topology') {
    if (state.dirty && state.task?.id !== id) { toast('当前拓扑有未保存的修改，请先保存配置。'); return; }
    closeTerminal();
    const data = await api(`/api/tasks/${encodeURIComponent(id)}`);
    state.task = data.task; state.spec = data.spec; state.selection = null; state.dirty = false;
    state.hostDetails = null; state.launchMode = null;
    state.recipe = (await api(`/api/tasks/${encodeURIComponent(id)}/recipe`)).recipe;
    state.selectedRun = null;
    $('#launch-preflight-list').hidden = true;
    $('#preflight-list').hidden = true;
    renderRecipe();
    renderTopology(); showPage(page);
    await refreshTopologyAvailability();
  }
  function defaultTask() {
    if (state.task) return true;
    toast('请先从任务列表选择一个任务。');
    showPage('tasks');
    return false;
  }
  async function syncCreateScale() {
    const request = ++createStatusRequest;
    $('#new-host-count').max = '8';
    $('#new-host-count').disabled = false;
    const count = Number($('#new-host-count').value);
    if (!Number.isInteger(count) || count < 2 || count > 8) {
      $('#new-scale-note').textContent = '请输入当前仿真器支持的主机数量。'; return;
    }
    $('#new-scale-note').textContent = '正在检查启动方式…';
    try {
      const status = await api('/api/topology/checkpoint', {method:'POST',
        body:JSON.stringify({simulator:$('#new-simulator').value,host_count:count})});
      if (request !== createStatusRequest) return;
      $('#new-scale-note').textContent = status.detail;
    } catch (error) {
      if (request === createStatusRequest) $('#new-scale-note').textContent = error.message;
    }
  }
  async function refreshTopologyAvailability() {
    if (!state.spec) return;
    const request = ++topologyStatusRequest;
    const taskId = state.task.id;
    $('#topology-readiness').classList.remove('ready');
    $('#topology-readiness').textContent = '正在检查启动方式…';
    $('#run-task').disabled = true;
    try {
      const status = await api('/api/topology/checkpoint', {method:'POST',
        body:JSON.stringify({spec:state.spec})});
      if (request !== topologyStatusRequest || state.task?.id !== taskId) return;
      state.hostDetails = status.hosts;
      state.launchMode = status.launch_mode;
      $('#topology-readiness').classList.toggle('ready', status.available);
      $('#topology-readiness').textContent = status.detail;
      $('#run-task').disabled = !status.available;
      renderInspector(); renderExecution();
    } catch (error) {
      if (request !== topologyStatusRequest || state.task?.id !== taskId) return;
      $('#topology-readiness').textContent = `检查失败：${error.message}`;
    }
  }
  function nodePosition(index, total, direct) {
    if (total === 2) return direct ? { x: index === 0 ? 220 : 780, y: 260 } :
      { x: index === 0 ? 200 : 800, y: 260 };
    const angle = (-Math.PI / 2) + (2 * Math.PI * index / total);
    return { x: 500 + 365 * Math.cos(angle), y: 260 + 175 * Math.sin(angle) };
  }
  function renderGraph() {
    const spec = state.spec;
    const direct = !spec.switches.length;
    const positions = spec.nodes.map((node, index) => nodePosition(index, spec.nodes.length, direct));
    const center = direct ? null : { x: 500, y: 260 };
    const deviceToNode = Object.fromEntries(spec.devices.map((device) =>
      [device.id, spec.nodes.findIndex((node) => node.id === device.node)]));
    let links = '';
    for (const [index, link] of spec.links.entries()) {
      const source = positions[deviceToNode[link.device]];
      const target = direct ? positions[deviceToNode[link.peer_device]] : center;
      const count = spec.links.filter((item) => item.device === link.device).length;
      const shift = (link.device_port - (count - 1) / 2) * 18;
      const x1 = source.x, y1 = source.y + shift, x2 = target.x, y2 = target.y + shift;
      const labelX = (x1 + x2) / 2, labelY = (y1 + y2) / 2 - 14;
      const selected = state.selection === `link:${index}`;
      links += `<g class="link ${selected ? 'selected' : ''}" data-select="link:${index}" role="button" tabindex="0" aria-label="查看连接 ${esc(link.device)} 端口 ${link.device_port} 的属性"><path class="wire-visible" d="M ${x1} ${y1} L ${x2} ${y2}"></path><path class="wire-hit" d="M ${x1} ${y1} L ${x2} ${y2}"></path><text class="wire-caption" x="${labelX}" y="${labelY}" text-anchor="middle">${link.rate_gbps}G · ${link.latency_ns}ns</text></g>`;
    }
    let html = `<svg viewBox="0 0 1000 520" preserveAspectRatio="none" aria-hidden="false">${links}</svg>`;
    spec.nodes.forEach((node, index) => {
      const pos = positions[index];
      const device = spec.devices.find((item) => item.node === node.id);
      html += `<div class="graph-node ${state.selection === `host:${index}` ? 'selected' : ''}" style="left:${pos.x / 10}%;top:${pos.y / 5.2}%"><button data-select="host:${index}" title="查看 ${esc(node.id)} 属性"><span class="node-name">${esc(node.id)}</span><span class="node-detail">UDMA · EID ${device?.eid ?? '—'}</span></button><button class="node-terminal" data-terminal="${esc(node.id)}">查看终端 ↗</button></div>`;
    });
    if (!direct) {
      html += `<div class="graph-node switch ${state.selection === 'switch:0' ? 'selected' : ''}" style="left:${center.x / 10}%;top:${center.y / 5.2}%"><button data-select="switch:0"><span class="node-name">交换芯片</span><span class="node-detail">ns-3-UB</span></button></div>`;
    }
    $('#graph').innerHTML = html;
    $('#canvas-count').textContent = `${spec.nodes.length} 主机 · ${spec.links.length} 条连接`;
    if (direct && spec.links.length) $('#path-summary').textContent = `单向网络路径：${spec.links[0].latency_ns} ns 直连传播，无交换处理。链路延迟在两个 UB-NET 边界分摊。`;
    else if (spec.nodes.length === 2 && spec.links.length === 2) {
      const a = spec.links[0].latency_ns, b = spec.links[1].latency_ns, s = spec.switches[0].delay_ns;
      $('#path-summary').textContent = `单向网络路径：${a} ns 链路 + ${s} ns 交换 + ${b} ns 链路 = ${a + s + b} ns 固定网络延迟（不含传输与主机处理）。`;
    } else $('#path-summary').textContent = '主机端口到交换端口的连接已按拓扑规则自动生成。点击连接或交换芯片调整延迟与速率。';
  }
  function readOnly(label, value, help = '') {
    return `<div class="property"><span class="prop-label">${esc(label)}</span><span class="prop-value">${esc(value)}</span>${help ? `<span class="prop-help">${esc(help)}</span>` : ''}</div>`;
  }
  function inspectorGroup(title) {
    return `<div class="inspector-group">${esc(title)}</div>`;
  }
  function editNumber(label, path, value, help = '') {
    return `<div class="property"><label for="edit-${esc(path)}">${esc(label)}</label><input id="edit-${esc(path)}" class="inspector-input" type="number" min="0" step="1" value="${esc(value)}" data-edit="${esc(path)}" ${state.task.editable ? '' : 'disabled'}>${help ? `<span class="prop-help">${esc(help)}</span>` : ''}</div>`;
  }
  function editSelect(label, path, value, options, help = '') {
    return `<div class="property"><label for="edit-${esc(path)}">${esc(label)}</label><select id="edit-${esc(path)}" class="inspector-select" data-edit="${esc(path)}" ${state.task.editable ? '' : 'disabled'}>${options.map((option) => `<option value="${esc(option)}" ${option === value ? 'selected' : ''}>${esc(option)}</option>`).join('')}</select>${help ? `<span class="prop-help">${esc(help)}</span>` : ''}</div>`;
  }
  function renderInspector() {
    $('#inspector').hidden = !state.selection;
    if (!state.selection) return;
    const spec = state.spec;
    const [kind, indexText] = state.selection.split(':');
    const index = Number(indexText);
    let html = '';
    if (kind === 'host') {
      const node = spec.nodes[index];
      const info = state.hostDetails?.[index];
      $('#selection-type').textContent = '主机';
      html += readOnly('主机', node.id);
      html += inspectorGroup('处理器');
      if (node.simulator === 'gem5') {
        html += editSelect('CPU 配置', 'node.profile', node.profile,
          ['modern','udma400','server','fast','kvm'], '同一任务的 gem5 主机使用相同配置。');
      } else {
        html += editSelect('QEMU CPU', `nodes.${index}.cpu`, node.cpu ?? 'cortex-a72',
          ['cortex-a53','cortex-a57','cortex-a72','cortex-a76','cortex-a710','neoverse-n1','neoverse-n2','neoverse-v1','max']);
        html += editNumber('CPU 核数', `nodes.${index}.cores`, node.cores ?? 1);
      }
      html += readOnly('运行时 CPU 模型', info?.cpu_model ?? '读取中…');
      if (info?.boot_cpu_model && info.boot_cpu_model !== info.cpu_model)
        html += readOnly('启动阶段 CPU', info.boot_cpu_model);
      if (node.simulator === 'gem5') html += readOnly('CPU 核数', info?.cpu_count ?? '读取中…');
      html += readOnly('CPU 频率', info?.cpu_freq ??
        (node.simulator === 'gem5' ? '读取中…' : '未固定（QEMU TCG）'));
      html += inspectorGroup('缓存');
      html += info?.l1i ? readOnly('L1 指令 / 数据', `${info.l1i} / ${info.l1d}`) +
        readOnly('L2', info.l2) + (info.l3 ? readOnly('L3', info.l3) : '') :
        readOnly('缓存容量', node.simulator === 'gem5' ? '读取中…' : 'QEMU 未显式建模');
      html += inspectorGroup('内存');
      if (node.simulator === 'gem5')
        html += editNumber('建模内存 GiB', 'machine.memory_size_gb', spec.machine?.memory_size_gb ?? 32,
          '同一任务的 gem5 主机使用相同容量。');
      else html += editNumber('内存 MiB', `nodes.${index}.memory_mib`, node.memory_mib ?? 1024);
      if (info?.guest_memory_limit) html += readOnly('客体内存上限', info.guest_memory_limit);
      html += readOnly('内存类型', info?.memory_type ?? (node.simulator === 'qemu' ? '未建模 DDR 时序' : '读取中…'));
      if (info?.memory_channels) html += readOnly('内存通道', info.memory_channels);
      html += inspectorGroup('系统');
      html += readOnly('操作系统 / 内核', info?.os ?? '读取中…');
      if (info?.page_size) html += readOnly('内核页大小', info.page_size);
      html += `<button class="button primary inspector-action" data-terminal="${esc(node.id)}">查看 ${esc(node.id)} 终端</button>`;
    } else if (kind === 'switch') {
      const sw = spec.switches[0];
      $('#selection-type').textContent = '交换芯片';
      html += readOnly('组件', sw.id) + readOnly('类型', 'ns-3-UB') +
        editNumber('转发处理延迟 ns', 'switches.0.delay_ns', sw.delay_ns,
          '在交换芯片入端口计时，每次转发计入一次。') +
        readOnly('端口数', spec.links.length, '每条连接占用一个交换端口。');
    } else if (kind === 'link') {
      const link = spec.links[index];
      const direct = !spec.switches.length;
      $('#selection-type').textContent = '连接';
      html += readOnly('连接', direct ? `${link.device}:${link.device_port} ↔ ${link.peer_device}:${link.peer_port}` :
        `${link.device}:${link.device_port} ↔ ${link.switch}:${link.switch_port}`);
      html += editNumber('端口速率 Gb/s', `links.${index}.rate_gbps`, link.rate_gbps);
      html += editNumber(direct ? '单向传播延迟 ns' : '该段传播延迟 ns',
        `links.${index}.latency_ns`, link.latency_ns,
        direct ? '直连总传播延迟在两个 UB-NET 边界分摊。' :
          '代表主机到交换芯片这一段；模型在该主机的 UB-NET 边界计时。');
    }
    if (!state.task.editable) html += `<div class="inspector-divider"></div><p class="prop-help">这是只读模板。新建任务后可保存修改。</p>`;
    $('#inspector-fields').innerHTML = html;
  }
  function renderExecution() {
    const spec = state.spec;
    const gem5 = spec.nodes[0].simulator === 'gem5';
    $('#execution-fields').innerHTML =
      readOnly('启动方式', gem5 ?
        (state.launchMode === 'restore' ? 'checkpoint 恢复' :
          state.launchMode === 'cold' ? '内核冷启动' : '自动选择 checkpoint 或冷启动') :
        'QEMU 功能 / 计时模式') +
      editSelect('同步计时', 'execution.sync', spec.execution.sync, ['off','required']) +
      readOnly('检查点策略', spec.execution.checkpoint);
  }
  function renderTopology() {
    if (!state.task) return;
    $('#topology-title').textContent = state.task.name;
    $('#topology-sub').textContent = `${state.task.simulator} · ${state.spec.nodes.length} 主机 · ${state.spec.switches.length ? '一颗 ns-3-UB 交换芯片' : 'UB 直连'}`;
    $('#save-task').hidden = !state.task.editable;
    $('#save-task').disabled = !state.dirty;
    $('#copy-task').hidden = !state.task;
    $('#dirty-badge').hidden = !state.dirty;
    $('#topology-hint').hidden = state.task.editable;
    if (!state.task.editable) $('#topology-hint').textContent = '当前是只读示例。可从“任务”页新建相同类型的任务后编辑。';
    $('#shape-host-count').value = state.spec.nodes.length;
    $('#shape-host-count').disabled = !state.task.editable || !state.spec.switches.length;
    $('#shape-topology').textContent = state.spec.switches.length ? '单叶交换机星型' : 'UB 直连（旧任务）';
    $('#shape-title').textContent = $('#shape-topology').textContent;
    $('#shape-leaf-count').textContent = state.spec.switches.length;
    $('#shape-note').textContent = !state.spec.switches.length ?
      '旧任务的直连拓扑可查看参数；新建任务使用单叶交换机星型。' :
      state.task.simulator === 'gem5' ?
        'gem5 支持 2–8 台主机。规模变化会重新生成连接；有兼容 checkpoint 时恢复，否则冷启动。' :
        'QEMU 支持 2–8 台主机，逐台设置 CPU、核心数和内存；启动前检查宿主内存。';
    renderGraph(); renderInspector(); renderExecution();
  }
  async function resizeTopology(value) {
    if (!state.task?.editable || !state.spec.switches.length) return;
    const count = Number(value);
    if (!Number.isInteger(count) || count < 2 || count > 8) {
      toast('主机数量超出当前仿真器支持范围。'); renderTopology(); return;
    }
    const result = await api('/api/topology/resize', {method:'POST',
      body:JSON.stringify({spec:state.spec,host_count:count})});
    state.spec = result.spec;
    if (count !== 2) state.recipe = null;
    if (state.selection) {
      const [kind, selectedIndex] = state.selection.split(':');
      if ((kind === 'host' && Number(selectedIndex) >= count) ||
          (kind === 'link' && Number(selectedIndex) >= state.spec.links.length))
        state.selection = null;
    }
    state.dirty = true;
    renderTopology(); renderRecipe(); await refreshTopologyAvailability();
    toast(`已生成 ${count} 台主机与 ${state.spec.links.length} 条连接。保存后生效。`);
  }
  async function copyTask(id) {
    if (state.dirty && state.task?.id !== id) {
      toast('当前拓扑有未保存的修改，请先保存配置。'); return;
    }
    if (state.dirty && state.task?.id === id) await saveTask();
    const result = await api(`/api/tasks/${encodeURIComponent(id)}/copy`,
      {method:'POST',body:'{}'});
    await refreshTasks(); await openTask(result.task.id);
    toast('任务拓扑与实验配方已复制，可以继续编辑。');
  }
  function setNested(path, value) {
    const parts = path.split('.');
    let cursor = state.spec;
    for (let i = 0; i < parts.length - 1; i++) {
      cursor[parts[i]] ??= {};
      cursor = cursor[parts[i]];
    }
    cursor[parts.at(-1)] = value;
  }
  function applyEdit(input) {
    if (!state.task?.editable) return;
    const path = input.dataset.edit;
    let value = input.type === 'number' ? Number(input.value) : input.value;
    if (input.type === 'number' && (!Number.isSafeInteger(value) || value < 0 || input.value === '')) {
      toast('请输入有效的非负整数。'); renderTopology(); return;
    }
    if (path === 'device.max_jettys') state.spec.devices.forEach((device) => { device.max_jettys = value; });
    else if (path === 'node.profile') state.spec.nodes.forEach((node) => { node.profile = value; });
    else setNested(path, value);
    state.hostDetails = null; state.launchMode = null;
    state.dirty = true; renderTopology();
    safely(() => refreshTopologyAvailability());
  }
  async function saveTask() {
    if (!state.task?.editable || !state.dirty) return;
    const body = { spec: state.spec, display_name: state.task.name };
    const result = await api(`/api/tasks/${encodeURIComponent(state.task.id)}`,
      { method: 'PUT', body: JSON.stringify(body) });
    state.task = result.task; state.dirty = false;
    state.recipe = (await api(`/api/tasks/${encodeURIComponent(state.task.id)}/recipe`)).recipe;
    renderRecipe(); renderTopology(); await refreshTasks();
    await refreshTopologyAvailability(); toast('拓扑已保存并通过配置校验。');
  }
  async function startTask(id) {
    if (state.dirty && state.task?.id !== id) throw new Error('当前拓扑有未保存的修改，请先保存配置。');
    if (state.dirty && state.task?.id === id) await saveTask();
    const verdict = await api(`/api/tasks/${encodeURIComponent(id)}/preflight`);
    if (!verdict.ok) {
      if (state.task?.id !== id) await openTask(id);
      else showPage('topology');
      showChecks('#launch-preflight-list', verdict);
      toast('启动前检查未通过，请查看具体原因。');
      return;
    }
    const result = await api(`/api/tasks/${encodeURIComponent(id)}/start`,
      { method: 'POST', body: '{}' });
    if (state.task?.id !== id) await openTask(id, 'analysis');
    else showPage('analysis');
    toast('仿真启动中，日志会自动更新。');
    state.runtime = null; await refreshRuntime();
    return result;
  }
  function renderRuntime() {
    const task = state.task;
    if (!task) return;
    const runtime = state.runtime;
    const current = runtime?.[task.simulator];
    const same = current?.topology_name === state.spec?.name;
    const job = runtime?.job?.task_id === task.id ? runtime.job : null;
    $('#run-sub').textContent = task.name;
    $('#run-simulator').textContent = task.simulator;
    $('#run-layout').textContent = task.layout === 'switch' ? '经交换芯片' : 'UB 直连';
    $('#run-nodes').textContent = String(task.node_count);
    $('#run-dir').textContent = current?.run_dir ? `运行目录：${current.run_dir}。拓扑与启动计划保存在该目录的 topology.json、topology-plan.json。` : '尚无运行目录。';
    $('#stop-run').hidden = !(same && current.active);
    if (job?.state === 'running') {
      $('#run-state').textContent = '启动中';
      $('#run-status-hint').textContent = '正在调用现有仿真启动器';
    } else if (same && current.active) {
      $('#run-state').textContent = '运行中';
      $('#run-status-hint').textContent = '可在拓扑中打开对应主机终端';
    } else if (same && current?.stop_reason) {
      $('#run-state').textContent = '内存保护已停止';
      $('#run-status-hint').textContent = current.stop_reason;
    } else if (job?.state === 'failed') {
      $('#run-state').textContent = '启动失败';
      $('#run-status-hint').textContent = `启动进程退出码 ${job.returncode}`;
    } else {
      $('#run-state').textContent = '未运行';
      $('#run-status-hint').textContent = job?.state === 'finished' ? '启动进程已结束' : '尚未从本页面启动';
    }
    $('#run-log').textContent = job?.log || (same ? '本次仿真由其他入口启动；可查看运行目录和主机终端。' : '暂无该任务的启动日志。');
  }
  async function refreshRuntime() {
    try { state.runtime = await api('/api/runtime'); renderRuntime(); }
    catch (error) { toast(error.message); }
  }
  function renderRecipe() {
    if (!state.recipe) {
      document.querySelectorAll('.experiment-card input').forEach(input => { input.disabled = true; });
      $('#save-recipe').disabled = true;
      $('#check-experiment').disabled = true;
      $('#start-experiment').disabled = true;
      $('#recipe-note').textContent = state.spec?.nodes.length === 2 ?
        '保存双机拓扑后即可配置性能实验。' :
        '性能实验当前支持双机拓扑。多主机拓扑可启动并使用节点终端；请选择双机任务运行内置实验。';
      return;
    }
    const r = state.recipe;
    document.querySelectorAll('[data-operation]').forEach(input => { input.checked = r.operations.includes(input.dataset.operation); });
    $('#recipe-sizes').value = r.sizes.join(','); $('#recipe-jettys').value = r.jettys.join(',');
    $('#recipe-ops-count').value = r.total_ops; $('#recipe-warmups').value = r.warmups;
    $('#recipe-repeats').value = r.repeats; $('#recipe-depth').value = r.depth;
    $('#recipe-jfc').value = r.jfc_depth; $('#recipe-post').value = r.post_list;
    $('#recipe-timeout').value = r.timeout_s;
    document.querySelectorAll('.experiment-card input').forEach(input => { input.disabled = false; });
    $('#save-recipe').disabled = false;
    $('#check-experiment').disabled = false;
    $('#start-experiment').disabled = false;
    $('#recipe-note').textContent = `${state.task.editable ? '' : '模板拓扑只读，实验配方可编辑。'}预热单独保存，不计入均值。延迟测试目前使用 1 个 Jetty。`;
  }
  function parseNumbers(selector) {
    return $(selector).value.split(/[,，\s]+/).filter(Boolean).map(value => {
      const number = Number(value);
      if (!Number.isSafeInteger(number)) throw new Error(`${selector} 需要整数列表`);
      return number;
    });
  }
  function recipeFromForm() {
    return {
      operations: [...document.querySelectorAll('[data-operation]:checked')].map(el => el.dataset.operation),
      sizes: parseNumbers('#recipe-sizes'), jettys: parseNumbers('#recipe-jettys'),
      total_ops: Number($('#recipe-ops-count').value), warmups: Number($('#recipe-warmups').value),
      repeats: Number($('#recipe-repeats').value), depth: Number($('#recipe-depth').value),
      jfc_depth: Number($('#recipe-jfc').value), post_list: Number($('#recipe-post').value),
      timeout_s: Number($('#recipe-timeout').value),
    };
  }
  async function saveRecipe() {
    if (!defaultTask()) return;
    if (!state.recipe) throw new Error('性能实验当前只支持双机拓扑。');
    const result = await api(`/api/tasks/${encodeURIComponent(state.task.id)}/recipe`,
      {method:'PUT', body:JSON.stringify(recipeFromForm())});
    state.recipe = result.recipe;
    $('#preflight-list').hidden = true;
    toast('实验配方已保存。');
  }
  async function checkExperiment() {
    if (!defaultTask()) return;
    if (!state.recipe) throw new Error('性能实验当前只支持双机拓扑。');
    const verdict = await api(`/api/tasks/${encodeURIComponent(state.task.id)}/preflight?experiment=1`);
    showChecks('#preflight-list', verdict);
    return verdict;
  }
  function showChecks(selector, verdict) {
    $(selector).hidden = false;
    $(selector).innerHTML = verdict.checks.map(item => `<div class="preflight-item ${item.ok ? 'ok' : 'bad'}"><span>${item.ok ? '✓' : '!'}</span><strong>${esc(item.label)}</strong><small>${esc(item.detail)}</small></div>`).join('');
  }
  function resultChart(rows) {
    if (!rows.length) return '';
    const width = 700, height = 220, left = 62, right = 22, top = 18, bottom = 34;
    const xmin = Math.min(...rows.map(row => row.size_bytes));
    const xmax = Math.max(...rows.map(row => row.size_bytes));
    const x = size => left + (Math.log2(size) - Math.log2(xmin)) / Math.max(1, Math.log2(xmax) - Math.log2(xmin)) * (width - left - right);
    let result = '';
    for (const operation of [...new Set(rows.map(row => row.operation))]) {
      const points = rows.filter(row => row.operation === operation);
      const values = points.map(row => row.value).filter(value => Number.isFinite(value) && value > 0);
      const ymin = Math.min(...values) * 0.8, ymax = Math.max(...values) * 1.2;
      const y = value => top + (Math.log(ymax) - Math.log(value)) / Math.max(0.01, Math.log(ymax) - Math.log(ymin)) * (height - top - bottom);
      const series = [...new Set(points.map(row => row.jettys))];
      let svg = `<svg viewBox="0 0 ${width} ${height}" role="img" aria-label="${esc(operation)} 随消息大小的变化"><path d="M${left} ${top}V${height-bottom}H${width-right}" fill="none" stroke="currentColor" opacity=".35"/>`;
      for (const [i, jettys] of series.entries()) {
        const sorted = points.filter(row => row.jettys === jettys).sort((a,b) => a.size_bytes - b.size_bytes);
        const color = ['#0a91a1','#aa6dc8','#d7873f','#55a366','#cc6971'][i % 5];
        svg += `<polyline points="${sorted.map(row => `${x(row.size_bytes)},${y(row.value)}`).join(' ')}" fill="none" stroke="${color}" stroke-width="2.7"/>`;
        svg += sorted.map(row => `<circle cx="${x(row.size_bytes)}" cy="${y(row.value)}" r="3.5" fill="${color}"><title>${esc(operation)} ${row.size_bytes}B J=${jettys}: ${row.value.toFixed(3)}</title></circle>`).join('');
      }
      svg += `<text x="${left}" y="${height-7}" fill="currentColor" font-size="11">${xmin}B</text><text x="${width-right}" y="${height-7}" text-anchor="end" fill="currentColor" font-size="11">${xmax}B</text><text x="8" y="${top+4}" fill="currentColor" font-size="10">${ymax.toFixed(1)}</text><text x="8" y="${height-bottom}" fill="currentColor" font-size="10">${ymin.toFixed(1)}</text></svg>`;
      result += `<section class="chart-panel"><div><strong>${esc(operation.toUpperCase())}</strong><small>${operation.endsWith('bw') ? 'MiB/s' : 'µs'} · 对数刻度</small></div>${svg}<div class="chart-legend">${series.map((j,i) => `<span style="--legend:${['#0a91a1','#aa6dc8','#d7873f','#55a366','#cc6971'][i % 5]}">J=${j}</span>`).join('')}</div></section>`;
    }
    return result;
  }
  function renderExperiment(run) {
    state.experiment = run;
    const label = {starting:'启动中', running:'运行中', finished:'已完成', failed:'失败'}[run.state] || run.state;
    $('#experiment-status').textContent = `${label} · ${run.completed ?? 0}/${run.total ?? '—'} 次 · ${run.current ?? ''}${run.error ? ' · ' + run.error : ''}${state.task?.simulator === 'qemu' ? ' · QEMU 结果仅用于功能验证' : ''}`;
    const rows = run.summary || [];
    $('#result-chart').hidden = !rows.length;
    $('#result-chart').innerHTML = resultChart(rows);
    $('#result-table').innerHTML = rows.length ? `<table><thead><tr><th>测试</th><th>大小</th><th>Jetty</th><th>重复</th><th>均值</th><th>最小</th><th>最大</th><th>包率</th></tr></thead><tbody>${rows.map(row => `<tr><td>${esc(row.operation)}</td><td>${row.size_bytes} B</td><td>${row.jettys}</td><td>${row.repeats}</td><td>${row.value.toFixed(3)} ${row.metric === 'average_us' ? 'µs' : 'MiB/s'}</td><td>${row.minimum.toFixed(3)}</td><td>${row.maximum.toFixed(3)}</td><td>${row.message_rate_mpps === undefined ? '—' : row.message_rate_mpps.toFixed(3) + ' Mpps'}</td></tr>`).join('')}</tbody></table>` : '<p class="subtle">尚无有效测量点；预热数据只保存在 CSV 中。</p>';
    $('#download-results').hidden = !rows.length;
    $('#download-results').href = `/api/experiments/${encodeURIComponent(run.id)}/results.csv`;
    $('#experiment-log').hidden = !run.log;
    $('#experiment-log').textContent = run.log || '';
  }
  async function refreshExperiments() {
    if (!state.task) return;
    const taskId = state.task.id;
    const listing = await api(`/api/experiments?task=${encodeURIComponent(taskId)}`);
    if (state.task?.id !== taskId) return;
    $('#run-select').innerHTML = listing.runs.length ? listing.runs.map(run => `<option value="${esc(run.id)}">${esc(run.id)} · ${esc(run.state)}</option>`).join('') : '<option value="">暂无实验</option>';
    if (!listing.runs.length) { state.selectedRun = null; return; }
    if (!listing.runs.some(run => run.id === state.selectedRun)) state.selectedRun = listing.runs[0].id;
    $('#run-select').value = state.selectedRun;
    renderExperiment(await api(`/api/experiments/${encodeURIComponent(state.selectedRun)}`));
  }
  function renderTerminal(scrollToMatch = false) {
    const output = $('#terminal-output');
    const query = $('#terminal-search').value.trim().toLowerCase();
    if (!query) {
      output.textContent = state.terminalText;
      state.searchMatches = []; $('#terminal-search-count').textContent = '—';
      return;
    }
    const lower = state.terminalText.toLowerCase();
    const matches = [];
    for (let from = 0; matches.length < 500; from += query.length) {
      const index = lower.indexOf(query, from);
      if (index < 0) break;
      matches.push(index); from = index;
    }
    state.searchMatches = matches;
    if (state.searchIndex >= matches.length) state.searchIndex = 0;
    $('#terminal-search-count').textContent = matches.length ?
      `${state.searchIndex + 1}/${matches.length}${matches.length === 500 ? '+' : ''}` : '0/0';
    let cursor = 0, html = '';
    matches.forEach((index, ordinal) => {
      html += esc(state.terminalText.slice(cursor, index));
      html += `<mark class="${ordinal === state.searchIndex ? 'current' : ''}">${esc(state.terminalText.slice(index, index + query.length))}</mark>`;
      cursor = index + query.length;
    });
    output.innerHTML = html + esc(state.terminalText.slice(cursor));
    if (scrollToMatch) output.querySelector('mark.current')?.scrollIntoView({block:'center'});
  }
  function appendTerminal(text) {
    const output = $('#terminal-output');
    const atBottom = output.scrollHeight - output.scrollTop - output.clientHeight < 40;
    let cleaned = '';
    for (const char of text) {
      if (state.terminalMode === 'text') {
        if (char === '\x1b') { state.terminalMode = 'escape'; continue; }
        if (char === '\r') { cleaned += '\n'; state.terminalCR = true; continue; }
        if (char === '\n' && state.terminalCR) { state.terminalCR = false; continue; }
        state.terminalCR = false; cleaned += char;
      } else if (state.terminalMode === 'escape') {
        state.terminalMode = char === '[' ? 'csi' : char === ']' ? 'osc' : 'text';
      } else if (state.terminalMode === 'csi') {
        if (char >= '@' && char <= '~') state.terminalMode = 'text';
      } else if (state.terminalMode === 'osc') {
        if (char === '\x07') state.terminalMode = 'text';
        else if (char === '\x1b') state.terminalMode = 'osc-end';
      } else state.terminalMode = char === '\\' ? 'text' : 'osc';
    }
    state.terminalText = (state.terminalText + cleaned).slice(-120000);
    renderTerminal();
    if (atBottom && !$('#terminal-search').value) output.scrollTop = output.scrollHeight;
  }
  function stepTerminalMatch(delta) {
    if (!state.searchMatches.length) return;
    state.searchIndex = (state.searchIndex + delta + state.searchMatches.length) % state.searchMatches.length;
    renderTerminal(true);
  }
  function closeTerminal() {
    if (state.socket) { state.socket.close(); state.socket = null; }
    state.terminalNode = null;
    $('#terminal-drawer').classList.remove('open');
    $('#terminal-drawer').setAttribute('aria-hidden', 'true');
    $('#terminal-drawer').inert = true;
  }
  async function openTerminal(nodeId) {
    if (!defaultTask()) return;
    closeTerminal();
    state.terminalNode = nodeId;
    const drawer = $('#terminal-drawer');
    drawer.inert = false; drawer.classList.add('open'); drawer.setAttribute('aria-hidden', 'false');
    $('#terminal-title').textContent = `${nodeId} 终端`;
    $('#terminal-state').textContent = '读取串口记录…';
    state.terminalText = ''; state.terminalMode = 'text'; state.terminalCR = false;
    state.searchIndex = 0;
    $('#terminal-search').value = ''; renderTerminal();
    $('#terminal-input').disabled = true;
    $('#terminal-form button').disabled = true;
    try {
      const result = await api(`/api/terminal?task=${encodeURIComponent(state.task.id)}&node=${encodeURIComponent(nodeId)}`);
      if (state.terminalNode !== nodeId) return;
      appendTerminal(result.text || '暂无串口输出。\n');
      if (!result.live) { $('#terminal-state').textContent = '离线 · 最近记录'; return; }
      $('#terminal-state').textContent = `连接到 ${result.simulator} 节点 ${result.node_index}…`;
      const url = `${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/ws/terminal?task=${encodeURIComponent(state.task.id)}&node=${encodeURIComponent(nodeId)}`;
      const socket = new WebSocket(url);
      state.socket = socket; state.terminalDecoder = new TextDecoder();
      socket.binaryType = 'arraybuffer';
      socket.onopen = () => {
        if (state.socket !== socket) return;
        $('#terminal-state').textContent = `已连接 · ${result.simulator} 节点 ${result.node_index}`;
        $('#terminal-input').disabled = false; $('#terminal-form button').disabled = false;
        $('#terminal-output').focus();
      };
      socket.onmessage = async (event) => {
        if (state.socket !== socket) return;
        const bytes = event.data instanceof ArrayBuffer ? event.data : await event.data.arrayBuffer();
        appendTerminal(state.terminalDecoder.decode(bytes, { stream: true }));
      };
      socket.onclose = () => {
        if (state.socket !== socket) return;
        $('#terminal-state').textContent = '终端连接已断开';
        $('#terminal-input').disabled = true; $('#terminal-form button').disabled = true;
        state.socket = null;
      };
      socket.onerror = () => { if (state.socket === socket) $('#terminal-state').textContent = '终端连接失败'; };
    } catch (error) { $('#terminal-state').textContent = error.message; }
  }
  async function safely(work) { try { await work(); } catch (error) { toast(error.message); } }

  document.addEventListener('click', (event) => {
    const el = event.target.closest('button,[data-select]');
    if (!el) return;
    safely(async () => {
      if (el.dataset.page) { if (el.dataset.page !== 'tasks' && !defaultTask()) return; showPage(el.dataset.page); }
      else if (el.id === 'new-toggle') $('#new-panel').hidden = !$('#new-panel').hidden;
      else if (el.id === 'new-close') $('#new-panel').hidden = true;
      else if (el.id === 'create-task') {
        const result = await api('/api/tasks', {method:'POST', body:JSON.stringify({
          name:$('#new-name').value, simulator:$('#new-simulator').value,
          topology_type:'single_leaf', host_count:Number($('#new-host-count').value) })});
        $('#new-panel').hidden = true; await refreshTasks(); await openTask(result.task.id);
        toast('拓扑已生成。点击节点、连线或交换芯片配置参数。');
      }
      else if (el.dataset.expand) { state.expanded = state.expanded === el.dataset.expand ? null : el.dataset.expand; await refreshTasks(); }
      else if (el.dataset.copy) await copyTask(el.dataset.copy);
      else if (el.dataset.open) await openTask(el.dataset.open);
      else if (el.dataset.results) await openTask(el.dataset.results, 'analysis');
      else if (el.dataset.start) await startTask(el.dataset.start);
      else if (el.id === 'save-task') await saveTask();
      else if (el.id === 'copy-task') { if (defaultTask()) await copyTask(state.task.id); }
      else if (el.id === 'run-task') { if (defaultTask()) await startTask(state.task.id); }
      else if (el.id === 'check-launch') {
        if (!defaultTask()) return;
        if (state.dirty) { toast('请先保存拓扑配置，再检查启动条件。'); return; }
        showChecks('#launch-preflight-list', await api(`/api/tasks/${encodeURIComponent(state.task.id)}/preflight`));
        await refreshTopologyAvailability();
      }
      else if (el.dataset.terminal) await openTerminal(el.dataset.terminal);
      else if (el.dataset.select) {
        state.selection = state.selection === el.dataset.select ? null : el.dataset.select;
        renderGraph(); renderInspector();
        if (state.selection) $('#inspector').scrollIntoView({behavior:'smooth', block:'start'});
        else $('#graph').scrollIntoView({behavior:'smooth', block:'center'});
      }
      else if (el.id === 'inspector-close') {
        state.selection = null; renderGraph(); renderInspector();
        $('#graph').scrollIntoView({behavior:'smooth', block:'center'});
      }
      else if (el.id === 'terminal-close') closeTerminal();
      else if (el.id === 'terminal-expand') {
        const wide = $('#terminal-drawer').classList.toggle('wide');
        el.setAttribute('aria-pressed', String(wide)); el.textContent = wide ? '收窄' : '宽屏';
      }
      else if (el.id === 'terminal-prev') stepTerminalMatch(-1);
      else if (el.id === 'terminal-next') stepTerminalMatch(1);
      else if (el.id === 'refresh-run') await refreshRuntime();
      else if (el.id === 'save-recipe') await saveRecipe();
      else if (el.id === 'check-experiment') await checkExperiment();
      else if (el.id === 'start-experiment') {
        if (!defaultTask()) return;
        await saveRecipe();
        const verdict = await checkExperiment();
        if (!verdict.ok) { toast('启动前检查未通过，请查看各项详情。'); return; }
        const started = await api(`/api/tasks/${encodeURIComponent(state.task.id)}/experiment`, {method:'POST',body:'{}'});
        state.selectedRun = started.run.id;
        toast('性能实验已启动。');
        await refreshExperiments();
      }
      else if (el.id === 'stop-run') {
        if (!state.task) return;
        const result = await api('/api/stop', {method:'POST', body:JSON.stringify({task_id:state.task.id})});
        toast(result.output || '仿真已停止。'); await refreshRuntime();
      }
    });
  });
  document.addEventListener('keydown', (event) => {
    const target = event.target.closest('[data-select][role="button"]');
    if (target && (event.key === 'Enter' || event.key === ' ')) { event.preventDefault(); target.click(); }
  });
  document.addEventListener('change', (event) => {
    if (event.target.dataset.edit) applyEdit(event.target);
    if (event.target.id === 'new-simulator') syncCreateScale();
    if (event.target.id === 'new-host-count') syncCreateScale();
    if (event.target.id === 'shape-host-count') safely(() => resizeTopology(event.target.value));
    if (event.target.id === 'run-select' && event.target.value) safely(async () => {
      state.selectedRun = event.target.value;
      renderExperiment(await api(`/api/experiments/${encodeURIComponent(state.selectedRun)}`));
    });
  });
  syncCreateScale();
  $('#terminal-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const value = $('#terminal-input').value;
    if (state.socket?.readyState !== WebSocket.OPEN || !value) return;
    state.socket.send(value + '\r'); $('#terminal-input').value = '';
  });
  $('#terminal-search').addEventListener('input', () => {
    state.searchIndex = 0; renderTerminal(true);
  });
  $('#terminal-search').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') { event.preventDefault(); stepTerminalMatch(event.shiftKey ? -1 : 1); }
  });
  $('#terminal-output').addEventListener('keydown', (event) => {
    if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'f') {
      event.preventDefault(); $('#terminal-search').focus(); return;
    }
    if (state.socket?.readyState !== WebSocket.OPEN) return;
    const special = {Enter:'\r',Backspace:'\x7f',Tab:'\t',ArrowUp:'\x1b[A',
      ArrowDown:'\x1b[B',ArrowRight:'\x1b[C',ArrowLeft:'\x1b[D',Delete:'\x1b[3~',Escape:'\x1b'};
    let bytes = special[event.key];
    if (event.ctrlKey && !event.metaKey && /^[a-z]$/i.test(event.key))
      bytes = String.fromCharCode(event.key.toUpperCase().charCodeAt(0) - 64);
    else if (!bytes && !event.ctrlKey && !event.metaKey && !event.altKey && event.key.length === 1)
      bytes = event.key;
    if (bytes) { event.preventDefault(); state.socket.send(bytes); }
  });
  $('#clock').textContent = new Intl.DateTimeFormat('zh-CN', {hour:'2-digit',minute:'2-digit'}).format(new Date());
  safely(async () => { await refreshTasks(); await refreshRuntime(); });
  setInterval(() => { if (state.page === 'analysis') { refreshRuntime(); refreshExperiments().catch(error => toast(error.message)); } }, 2500);
})();
