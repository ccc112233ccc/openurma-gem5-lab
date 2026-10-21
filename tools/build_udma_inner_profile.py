#!/usr/bin/env python3
"""Build an isolated ARM64 UDMA provider with batch-level post/poll timing."""

import argparse
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "artifacts/provider-inner-profile"
SOURCE = ROOT / "sources/umdk/src/urma/hw/udma"
TARGET = ROOT / "artifacts/umdk-build/urma/hw/udma"


def one(data: str, old: str, new: str) -> str:
    if data.count(old) != 1:
        raise RuntimeError(f"expected exactly one marker, got {data.count(old)}: {old[:80]!r}")
    return data.replace(old, new)


HEADER = r'''
#ifndef UBSIM_UDMA_INNER_PROFILE_H
#define UBSIM_UDMA_INNER_PROFILE_H
#include <stdint.h>
#include <stdbool.h>
struct ubsim_udma_inner_profile {
    uint64_t post_calls, post_wrs, post_lock_calls;
    uint64_t post_total_ticks, post_encode_ticks, post_barrier_ticks, post_db_ticks;
    uint64_t poll_calls, poll_cqes, poll_empty_calls, poll_lock_calls;
    uint64_t poll_total_ticks, poll_scan_ticks, poll_db_ticks;
    uint64_t cqe_samples, cqe_lookup_ticks, cqe_barrier_ticks, cqe_parse_ticks;
    uint64_t cqe_timer_pair_ticks;
    uint64_t cqe_relookup_ticks;
    uint64_t wqe_samples, wqe_check_ticks, wqe_set_ticks, wqe_queue_ticks;
    uint64_t wqe_timer_pair_ticks;
    uint64_t sqe_samples, sqe_check_ticks, sqe_prepare_ticks, sqe_parse_ticks;
};
extern struct ubsim_udma_inner_profile ubsim_inner;
extern bool ubsim_inner_on;
static inline uint64_t ubsim_inner_ticks(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}
static inline uint64_t ubsim_inner_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}
#endif
'''

JFS_GLOBALS = r'''
struct ubsim_udma_inner_profile ubsim_inner;
bool ubsim_inner_on;

__attribute__((constructor)) static void ubsim_inner_enable(void)
{
    ubsim_inner_on = getenv("UBSIM_UDMA_INNER_PROFILE") != NULL;
}

__attribute__((destructor)) static void ubsim_inner_emit(void)
{
    if (!ubsim_inner_on)
        return;
    fprintf(stderr,
        "[UDMA_INNER] frequency_hz=%llu post_calls=%llu post_wrs=%llu "
        "post_lock_calls=%llu post_total_ticks=%llu post_encode_ticks=%llu "
        "post_barrier_ticks=%llu post_db_ticks=%llu poll_calls=%llu "
        "poll_cqes=%llu poll_empty_calls=%llu poll_lock_calls=%llu "
        "poll_total_ticks=%llu poll_scan_ticks=%llu poll_db_ticks=%llu "
        "cqe_samples=%llu cqe_lookup_ticks=%llu cqe_barrier_ticks=%llu "
        "cqe_parse_ticks=%llu cqe_timer_pair_ticks=%llu cqe_relookup_ticks=%llu "
        "wqe_samples=%llu wqe_check_ticks=%llu wqe_set_ticks=%llu "
        "wqe_queue_ticks=%llu wqe_timer_pair_ticks=%llu "
        "sqe_samples=%llu sqe_check_ticks=%llu sqe_prepare_ticks=%llu "
        "sqe_parse_ticks=%llu\n",
        (unsigned long long)ubsim_inner_frequency(),
        (unsigned long long)ubsim_inner.post_calls,
        (unsigned long long)ubsim_inner.post_wrs,
        (unsigned long long)ubsim_inner.post_lock_calls,
        (unsigned long long)ubsim_inner.post_total_ticks,
        (unsigned long long)ubsim_inner.post_encode_ticks,
        (unsigned long long)ubsim_inner.post_barrier_ticks,
        (unsigned long long)ubsim_inner.post_db_ticks,
        (unsigned long long)ubsim_inner.poll_calls,
        (unsigned long long)ubsim_inner.poll_cqes,
        (unsigned long long)ubsim_inner.poll_empty_calls,
        (unsigned long long)ubsim_inner.poll_lock_calls,
        (unsigned long long)ubsim_inner.poll_total_ticks,
        (unsigned long long)ubsim_inner.poll_scan_ticks,
        (unsigned long long)ubsim_inner.poll_db_ticks,
        (unsigned long long)ubsim_inner.cqe_samples,
        (unsigned long long)ubsim_inner.cqe_lookup_ticks,
        (unsigned long long)ubsim_inner.cqe_barrier_ticks,
        (unsigned long long)ubsim_inner.cqe_parse_ticks,
        (unsigned long long)ubsim_inner.cqe_timer_pair_ticks,
        (unsigned long long)ubsim_inner.cqe_relookup_ticks,
        (unsigned long long)ubsim_inner.wqe_samples,
        (unsigned long long)ubsim_inner.wqe_check_ticks,
        (unsigned long long)ubsim_inner.wqe_set_ticks,
        (unsigned long long)ubsim_inner.wqe_queue_ticks,
        (unsigned long long)ubsim_inner.wqe_timer_pair_ticks,
        (unsigned long long)ubsim_inner.sqe_samples,
        (unsigned long long)ubsim_inner.sqe_check_ticks,
        (unsigned long long)ubsim_inner.sqe_prepare_ticks,
        (unsigned long long)ubsim_inner.sqe_parse_ticks);
}
'''


def patch_jfs(data: str) -> str:
    data = one(data, '#include "udma_u_jfs.h"',
               '#include "udma_u_jfs.h"\n#include "ubsim_udma_inner_profile.h"\n' + JFS_GLOBALS)
    begin = data.index('urma_status_t udma_u_post_sq_wr(')
    end = data.index('urma_status_t udma_u_post_jfs_wr(', begin)
    body = data[begin:end]
    body = one(body, '\tint wr_cnt = 0;\n',
               '\tint wr_cnt = 0;\n'
               '\tuint64_t profile_start = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\tuint64_t encode_start = 0, encode_end = 0, phase_start = 0;\n'
               '\tif (ubsim_inner_on) {\n'
               '\t\tubsim_inner.post_calls++;\n'
               '\t\tif (!sq->lock_free) ubsim_inner.post_lock_calls++;\n'
               '\t}\n')
    body = one(body, '\tfor (it = wr; it != NULL; it = (urma_jfs_wr_t *)(void *)it->next) {',
               '\tencode_start = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\tfor (it = wr; it != NULL; it = (urma_jfs_wr_t *)(void *)it->next) {')
    body = one(body, '\t\twr_cnt++;\n\t}\n\n\tif (wr_cnt) {',
               '\t\twr_cnt++;\n\t}\n'
               '\tencode_end = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\tif (ubsim_inner_on) {\n'
               '\t\tubsim_inner.post_wrs += wr_cnt;\n'
               '\t\tubsim_inner.post_encode_ticks += encode_end - encode_start;\n'
               '\t}\n\n\tif (wr_cnt) {')
    body = one(body, '\t\tUDMA_TO_DEVICE_BARRIER();\n\n'
               '\t\tif (wr_cnt == 1 && dwqe_enable && (sq->pi - sq->ci == 1))',
               '\t\tphase_start = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\t\tUDMA_TO_DEVICE_BARRIER();\n'
               '\t\tif (ubsim_inner_on) ubsim_inner.post_barrier_ticks += ubsim_inner_ticks() - phase_start;\n\n'
               '\t\tphase_start = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\t\tif (wr_cnt == 1 && dwqe_enable && (sq->pi - sq->ci == 1))')
    body = one(body, '\t\telse\n\t\t\tudma_update_sq_db(sq);\n\t}\n\n',
               '\t\telse\n\t\t\tudma_update_sq_db(sq);\n'
               '\t\tif (ubsim_inner_on) ubsim_inner.post_db_ticks += ubsim_inner_ticks() - phase_start;\n'
               '\t}\n\n')
    body = one(body, '\treturn ret;\n}',
               '\tif (ubsim_inner_on) ubsim_inner.post_total_ticks += ubsim_inner_ticks() - profile_start;\n'
               '\treturn ret;\n}')
    return data[:begin] + body + data[end:]


def patch_wqe_sample(data: str) -> str:
    begin = data.index('urma_status_t udma_u_post_one_wr(')
    end = data.index('urma_status_t udma_u_post_sq_wr(', begin)
    body = data[begin:end]
    body = one(body, '\tuint32_t i;\n',
               '\tuint32_t i;\n'
               '\tstatic uint64_t sample_counter;\n'
               '\tbool sample = false;\n'
               '\tif (ubsim_inner_on && ++sample_counter == 127u) {\n'
               '\t\tsample_counter = 0;\n'
               '\t\tsample = true;\n'
               '\t}\n'
               '\tif (sample) {\n'
               '\t\tuint64_t pair_start = ubsim_inner_ticks();\n'
               '\t\tubsim_inner.wqe_timer_pair_ticks += ubsim_inner_ticks() - pair_start;\n'
               '\t}\n'
               '\tuint64_t start = sample ? ubsim_inner_ticks() : 0;\n'
               '\tuint64_t check_end = 0, set_end = 0;\n')
    body = one(body, '\t\treturn URMA_EINVAL;\n\t}\n\n'
               '\tret = udma_set_sqe(',
               '\t\treturn URMA_EINVAL;\n\t}\n'
               '\tcheck_end = sample ? ubsim_inner_ticks() : 0;\n\n'
               '\tret = udma_set_sqe(')
    body = one(body, '\tif (ret)\n\t\treturn ret;\n\n'
               '\twqebb_cnt = wqe_info.wqe_cnt;',
               '\tif (ret)\n\t\treturn ret;\n'
               '\tset_end = sample ? ubsim_inner_ticks() : 0;\n\n'
               '\twqebb_cnt = wqe_info.wqe_cnt;')
    body = one(body, '\tsq->pi += wqebb_cnt;\n\n'
               '\treturn URMA_SUCCESS;',
               '\tsq->pi += wqebb_cnt;\n'
               '\tif (sample) {\n'
               '\t\tuint64_t queue_end = ubsim_inner_ticks();\n'
               '\t\tubsim_inner.wqe_samples++;\n'
               '\t\tubsim_inner.wqe_check_ticks += check_end - start;\n'
               '\t\tubsim_inner.wqe_set_ticks += set_end - check_end;\n'
               '\t\tubsim_inner.wqe_queue_ticks += queue_end - set_end;\n'
               '\t}\n\n'
               '\treturn URMA_SUCCESS;')
    return data[:begin] + body + data[end:]


def patch_sqe_sample(data: str) -> str:
    begin = data.index('static urma_status_t udma_set_sqe(')
    end = data.index('urma_status_t udma_u_post_one_wr(', begin)
    body = data[begin:end]
    body = one(body, '\turma_target_jetty_t *tjetty;\n',
               '\turma_target_jetty_t *tjetty;\n'
               '\tstatic uint64_t sample_counter;\n'
               '\tbool sample = false;\n'
               '\tif (ubsim_inner_on && ++sample_counter == 127u) {\n'
               '\t\tsample_counter = 0;\n'
               '\t\tsample = true;\n'
               '\t}\n'
               '\tuint64_t start = sample ? ubsim_inner_ticks() : 0;\n'
               '\tuint64_t check_end = 0, prepare_end = 0;\n')
    body = one(body, '\t\treturn URMA_EINVAL;\n\t}\n\n'
               '\t(void)memset(wqe_ctl, 0, sizeof(*wqe_ctl));',
               '\t\treturn URMA_EINVAL;\n\t}\n'
               '\tcheck_end = sample ? ubsim_inner_ticks() : 0;\n\n'
               '\t(void)memset(wqe_ctl, 0, sizeof(*wqe_ctl));')
    body = one(body, '\twqe_ctl->rmt_jetty_type = (uint8_t)(tjetty->type);\n'
               '\tif (udma_parse_jfs_wr(wqe_ctl, wr, sq, wqe_info, tjetty) != 0) {',
               '\twqe_ctl->rmt_jetty_type = (uint8_t)(tjetty->type);\n'
               '\tprepare_end = sample ? ubsim_inner_ticks() : 0;\n'
               '\tif (udma_parse_jfs_wr(wqe_ctl, wr, sq, wqe_info, tjetty) != 0) {')
    body = one(body, '\t\treturn URMA_EINVAL;\n\t}\n\n'
               '\treturn URMA_SUCCESS;\n}',
               '\t\treturn URMA_EINVAL;\n\t}\n'
               '\tif (sample) {\n'
               '\t\tuint64_t parse_end = ubsim_inner_ticks();\n'
               '\t\tubsim_inner.sqe_samples++;\n'
               '\t\tubsim_inner.sqe_check_ticks += check_end - start;\n'
               '\t\tubsim_inner.sqe_prepare_ticks += prepare_end - check_end;\n'
               '\t\tubsim_inner.sqe_parse_ticks += parse_end - prepare_end;\n'
               '\t}\n\n'
               '\treturn URMA_SUCCESS;\n}')
    return data[:begin] + body + data[end:]


def patch_jfc(data: str) -> str:
    data = one(data, '#include "udma_u_jfc.h"',
               '#include "udma_u_jfc.h"\n#include "ubsim_udma_inner_profile.h"')
    begin = data.index('int udma_u_poll_jfc(')
    end = data.index('int udma_u_wait_jfc(', begin)
    body = data[begin:end]
    body = one(body, '\tint npolled;\n',
               '\tint npolled;\n'
               '\tuint64_t profile_start = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\tuint64_t scan_start = 0, db_start = 0;\n'
               '\tif (ubsim_inner_on) {\n'
               '\t\tubsim_inner.poll_calls++;\n'
               '\t\tif (!udma_u_jfc->cq.lock_free) ubsim_inner.poll_lock_calls++;\n'
               '\t}\n')
    body = one(body, '\tfor (npolled = 0; npolled < cr_cnt; ++npolled) {',
               '\tscan_start = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\tfor (npolled = 0; npolled < cr_cnt; ++npolled) {')
    body = one(body, '\t\tif (ret != JFC_OK)\n\t\t\tbreak;\n\t}\n\n\tif (npolled) {',
               '\t\tif (ret != JFC_OK)\n\t\t\tbreak;\n\t}\n'
               '\tif (ubsim_inner_on) {\n'
               '\t\tubsim_inner.poll_scan_ticks += ubsim_inner_ticks() - scan_start;\n'
               '\t\tubsim_inner.poll_cqes += npolled;\n'
               '\t\tif (npolled == 0) ubsim_inner.poll_empty_calls++;\n'
               '\t}\n\n\tif (npolled) {')
    body = one(body, '\t\tci = udma_u_jfc->cq.ci;\n'
               '\t\t*udma_u_jfc->sw_db = ci & UDMA_U_JFC_DB_CI_IDX_M;\n',
               '\t\tdb_start = ubsim_inner_on ? ubsim_inner_ticks() : 0;\n'
               '\t\tci = udma_u_jfc->cq.ci;\n'
               '\t\t*udma_u_jfc->sw_db = ci & UDMA_U_JFC_DB_CI_IDX_M;\n'
               '\t\tif (ubsim_inner_on) ubsim_inner.poll_db_ticks += ubsim_inner_ticks() - db_start;\n')
    body = one(body, '\treturn ret == JFC_POLL_ERR ? -UDMA_INTER_ERR : npolled;\n}',
               '\tif (ubsim_inner_on) ubsim_inner.poll_total_ticks += ubsim_inner_ticks() - profile_start;\n'
               '\treturn ret == JFC_POLL_ERR ? -UDMA_INTER_ERR : npolled;\n}')
    return data[:begin] + body + data[end:]


def patch_cqe_sample(data: str, relookup: bool = False) -> str:
    begin = data.index('static enum jfc_poll_state udma_u_poll_one(')
    end = data.index('int udma_u_poll_jfc(', begin)
    body = data[begin:end]
    body = one(body, '\tstruct udma_u_jfc_cqe *cqe = get_next_cqe(udma_u_jfc, udma_u_jfc->cq.ci);',
               '\tstatic uint64_t sample_counter;\n'
               '\tbool sample = false;\n'
               '\tif (ubsim_inner_on && ++sample_counter == 127u) {\n'
               '\t\tsample_counter = 0;\n'
               '\t\tsample = true;\n'
               '\t}\n'
               '\tif (sample) {\n'
               '\t\tuint64_t pair_start = ubsim_inner_ticks();\n'
               '\t\tubsim_inner.cqe_timer_pair_ticks += ubsim_inner_ticks() - pair_start;\n'
               '\t}\n'
               '\tuint64_t start = sample ? ubsim_inner_ticks() : 0;\n'
               '\tstruct udma_u_jfc_cqe *cqe = get_next_cqe(udma_u_jfc, udma_u_jfc->cq.ci);\n'
               '\tuint64_t lookup_end = sample ? ubsim_inner_ticks() : 0;')
    if relookup:
        body = one(body, '\tuint64_t lookup_end = sample ? ubsim_inner_ticks() : 0;\n'
                   '\tif (cqe == NULL)',
                   '\tuint64_t lookup_end = sample ? ubsim_inner_ticks() : 0;\n'
                   '\tif (sample && cqe != NULL) {\n'
                   '\t\t__asm__ volatile("" ::: "memory");\n'
                   '\t\tuint64_t reload_start = ubsim_inner_ticks();\n'
                   '\t\tstruct udma_u_jfc_cqe *again = get_next_cqe(udma_u_jfc, udma_u_jfc->cq.ci);\n'
                   '\t\t__asm__ volatile("" :: "r"(again) : "memory");\n'
                   '\t\tubsim_inner.cqe_relookup_ticks += ubsim_inner_ticks() - reload_start;\n'
                   '\t}\n'
                   '\tif (cqe == NULL)')
    body = one(body, '\tUDMA_FROM_DEVICE_BARRIER();\n\t++udma_u_jfc->cq.ci;',
               '\tUDMA_FROM_DEVICE_BARRIER();\n'
               '\tuint64_t barrier_end = sample ? ubsim_inner_ticks() : 0;\n'
               '\t++udma_u_jfc->cq.ci;')
    body = one(body, '\tif (udma_u_parse_cqe_for_jfc(udma_ctx, cqe, cr))\n'
               '\t\treturn JFC_POLL_ERR;\n\n',
               '\tif (udma_u_parse_cqe_for_jfc(udma_ctx, cqe, cr))\n'
               '\t\treturn JFC_POLL_ERR;\n'
               '\tif (sample) {\n'
               '\t\tuint64_t parse_end = ubsim_inner_ticks();\n'
               '\t\tubsim_inner.cqe_samples++;\n'
               '\t\tubsim_inner.cqe_lookup_ticks += lookup_end - start;\n'
               '\t\tubsim_inner.cqe_barrier_ticks += barrier_end - lookup_end;\n'
               '\t\tubsim_inner.cqe_parse_ticks += parse_end - barrier_end;\n'
               '\t}\n\n')
    return data[:begin] + body + data[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample-cqe', action='store_true',
                        help='sample one of every 127 CQE polls')
    parser.add_argument('--sample-wqe', action='store_true',
                        help='sample one of every 127 WQE encodes')
    parser.add_argument('--sample-sqe', action='store_true',
                        help='sample the overflow/prepare/parse stages inside SQE construction')
    parser.add_argument('--sample-relookup', action='store_true',
                        help='immediately recheck the sampled CQE owner after its first lookup')
    args = parser.parse_args()
    if args.sample_relookup and not args.sample_cqe:
        parser.error('--sample-relookup requires --sample-cqe')
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / 'ubsim_udma_inner_profile.h').write_text(HEADER)
    cmake = TARGET / 'CMakeFiles/BETA_UDMA_SO.dir'
    values = {}
    for line in (cmake / 'flags.make').read_text().splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            values[key] = shlex.split(value)
    flags = values['C_DEFINES'] + values['C_INCLUDES'] + values['C_FLAGS']
    flags += [f'-I{BUILD}', f'-I{SOURCE}', f'-I{ROOT / "deps/ummu/include"}',
              f'-I{ROOT / "deps/ummu/kernel_headers"}']
    objects = {}
    for name, patcher in [('udma_u_jfs.c', patch_jfs), ('udma_u_jfc.c', patch_jfc)]:
        source = BUILD / name
        modified = patcher((SOURCE / name).read_text())
        if name == 'udma_u_jfs.c' and args.sample_wqe:
            modified = patch_wqe_sample(modified)
        if name == 'udma_u_jfs.c' and args.sample_sqe:
            modified = patch_sqe_sample(modified)
        if name == 'udma_u_jfc.c' and args.sample_cqe:
            modified = patch_cqe_sample(modified, args.sample_relookup)
        source.write_text(modified)
        suffix = '.sampled_relookup.o' if args.sample_relookup else '.sampled_deep.o' if args.sample_sqe else '.sampled_both.o' if args.sample_cqe and args.sample_wqe else (
            '.sampled_cqe.o' if args.sample_cqe else '.sampled_wqe.o' if args.sample_wqe else '.o')
        obj = BUILD / (name + suffix)
        subprocess.run(['gcc', *flags, '-c', str(source), '-o', str(obj)], check=True)
        objects[f'CMakeFiles/BETA_UDMA_SO.dir/{name}.o'] = str(obj)
    link = shlex.split((cmake / 'link.txt').read_text())
    for original, replacement in objects.items():
        link[link.index(original)] = replacement
    output_name = ('liburma-udma-sampled-relookup.so' if args.sample_relookup else
                   'liburma-udma-sampled-deep.so' if args.sample_sqe else
                   'liburma-udma-sampled-both.so' if args.sample_cqe and args.sample_wqe else
                   'liburma-udma-sampled.so' if args.sample_cqe else
                   'liburma-udma-wqe-sampled.so' if args.sample_wqe else 'liburma-udma.so')
    output = BUILD / output_name
    link[link.index('-o') + 1] = str(output)
    link.insert(link.index('-lummu'), f'-L{ROOT / "artifacts/umdk-build-ummu-official"}')
    subprocess.run(link, cwd=TARGET, check=True)
    print(output, output.stat().st_size)


if __name__ == '__main__':
    main()
