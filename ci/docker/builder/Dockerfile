FROM rockylinux:8

ARG ZF_DOCKERFILE_SHA=unknown

RUN dnf -y update && \
    dnf -y install dnf-plugins-core && \
    dnf config-manager --set-enabled powertools && \
    dnf -y install \
      cmake \
      git \
      gcc-toolset-12 \
      ninja-build \
      perl \
      python3.12 \
      unzip \
      zip && \
    alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 120 && \
    alternatives --set python3 /usr/bin/python3.12 && \
    dnf clean all

WORKDIR /work

ENV PATH=/opt/rh/gcc-toolset-12/root/usr/bin:$PATH
ENV CC=gcc
ENV CXX=g++
LABEL org.zfleet.dockerfile.sha=$ZF_DOCKERFILE_SHA
