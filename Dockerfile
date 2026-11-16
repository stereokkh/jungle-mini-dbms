FROM debian:bookworm-slim

# sqlite3: 동작 비교용 실제 DBMS
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev make gdb sqlite3 \
       curl ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

# Claude Code CLI.
# 컨테이너는 root 가 아닌 HOST_UID 로 실행되므로(compose.yaml), 설치 스크립트가
# 홈 디렉터리에 받은 실행 파일을 누구나 실행할 수 있는 /usr/local/bin 으로 옮긴다.
RUN export HOME=/opt/claude-install \
    && mkdir -p "$HOME" \
    && curl -fsSL https://claude.ai/install.sh | bash \
    && cp -L "$HOME/.local/bin/claude" /usr/local/bin/claude \
    && chmod 755 /usr/local/bin/claude \
    && rm -rf "$HOME"

# 로그인 정보·설정이 저장되는 곳. devcontainer 에서 named volume 을 여기에 붙인다.
RUN mkdir -p /claude-config && chmod 1777 /claude-config
ENV CLAUDE_CONFIG_DIR=/claude-config \
    DISABLE_AUTOUPDATER=1

WORKDIR /workspace

CMD ["bash"]
