# Jungle Mini DBMS

C로 작게 구현하는 DBMS입니다. 팀원은 Docker로 Debian 12, GCC 13, Make, GDB, SQLite가 포함된 C 개발 환경을 실행할 수 있습니다.

## 1. Docker 준비

macOS와 Windows에서는 Docker Desktop을 실행합니다. Windows에서는 Docker Desktop의 WSL 통합을 켜고 WSL 터미널에서 아래 명령을 실행합니다. Linux에서는 Docker Engine과 Docker Compose가 설치되어 있어야 합니다.

```sh
docker info
docker compose version
```

## 2. 저장소 받기 및 이미지 빌드

```sh
git clone https://github.com/stereokkh/jungle-mini-dbms.git
cd jungle-mini-dbms
docker compose build dev
```

## 3. 개발 셸 실행

```sh
HOST_UID=$(id -u) HOST_GID=$(id -g) docker compose run --rm dev
```

셸은 컨테이너의 `/workspace`에서 시작합니다. 이 경로는 로컬 저장소와 연결되어 있어 로컬 편집기에서 저장한 파일을 컨테이너에서 바로 사용할 수 있습니다. `exit`를 입력하면 컨테이너가 삭제되지만 저장소의 파일은 남습니다. `Dockerfile`이 바뀌면 `docker compose build dev`를 다시 실행합니다.

VS Code를 쓴다면 Dev Containers 확장으로 **Reopen in Container**를 눌러 같은 환경에서 열 수 있습니다.

## 4. 빌드와 테스트

개발 셸(`/workspace`) 안에서 실행합니다.

```sh
make            # build/mini-dbms
./build/mini-dbms
make test       # tests/test_*.c 를 각각 빌드해서 실행
make asan       # AddressSanitizer/UBSan 으로 테스트
make clean
```

`src/*.c`와 `tests/test_*.c`는 Makefile을 고치지 않아도 자동으로 포함됩니다. 동작을 비교할 때는 컨테이너 안의 `sqlite3`를 사용합니다.

## 5. 브랜치 전략

| 브랜치 | 용도 |
| --- | --- |
| `main` | 완성본. `develop`에서 검증된 내용만 머지합니다. |
| `develop` | 통합 브랜치. 모든 기능 PR은 여기로 보냅니다. |
| `feat/<이름>-<기능>` | 개인 작업 브랜치. `develop`에서 만들고 `develop`으로 PR을 엽니다. |

```sh
git switch develop
git pull
git switch -c feat/<이름>-<기능>
# 작업 후
git push -u origin feat/<이름>-<기능>
```

커밋 메시지는 `feat:`, `fix:`, `test:`, `docs:`, `chore:`, `refactor:` 접두어를 사용합니다.
