FROM rockylinux:8

RUN dnf -y update && \
    dnf -y install \
      ca-certificates \
      curl \
      libstdc++ \
      openssl-libs \
      unzip && \
    dnf clean all

WORKDIR /work
