FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

COPY docker/ubuntu-22.04-packages.txt /tmp/ubsim-ubuntu-packages.txt
RUN apt-get update \
 && xargs -r apt-get install -y --no-install-recommends \
      < /tmp/ubsim-ubuntu-packages.txt \
 && rm -rf /var/lib/apt/lists/* /tmp/ubsim-ubuntu-packages.txt

WORKDIR /workspace/ubsim-gem5-lab
CMD ["sleep", "infinity"]
