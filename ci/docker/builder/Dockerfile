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
      python3 \
      unzip \
      zip && \
    dnf clean all

WORKDIR /work

ENV PATH=/opt/rh/gcc-toolset-12/root/usr/bin:$PATH
ENV CC=gcc
ENV CXX=g++
LABEL org.zfleet.dockerfile.sha=$ZF_DOCKERFILE_SHA
