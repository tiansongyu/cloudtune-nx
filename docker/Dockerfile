# Official devkitPro multi-architecture image, pinned to its published digest.
# Update deliberately, then retain build/validation/toolchain-packages.txt.
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

LABEL org.opencontainers.image.title="Netease Switch development toolchain"
LABEL org.opencontainers.image.source="https://github.com/devkitPro/docker"

# switch-portlibs in the upstream image supplies the complete graphics, HTTP,
# crypto and audio stack. Verify requirements without an unpinned system upgrade.
RUN dkp-pacman -Q devkitA64 libnx switch-cmake switch-curl \
    switch-mbedtls switch-zlib switch-mesa switch-glfw switch-glm \
    && command -v cmake && command -v ninja && command -v elf2nro

ENV DEVKITPRO=/opt/devkitpro
ENV DEVKITA64=/opt/devkitpro/devkitA64
ENV PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:${PATH}
WORKDIR /workspace
CMD ["/bin/bash"]
