# CLAUDE.md — 미니 DBMS

C++17로 구현하는 3단계 미니 DBMS (SQL 처리기 → B+ 트리 인덱스 → Thread Pool API 서버).
포트폴리오 수준의 완성도가 목표이며, 핵심 로직은 설명 가능해야 한다.

## 절대 규칙 (위반 금지)

1. **외부 라이브러리 금지**: HTTP 서버/스레드 풀/B+ 트리/SQL 파서는 직접 구현한다. (표준 라이브러리 + POSIX 소켓만 사용. 테스트도 자체 하네스 사용)
2. **SQL 처리기 단일 경로**: CLI·벤치마크·API 서버 모두 동일한 엔진(`libminidb`)의 `Engine::execute()`를 통해서만 데이터에 접근한다. 저장소/인덱스를 우회하는 직접 접근 금지 (벌크 로더 제외, 반드시 같은 락을 거칠 것).
3. **동시성 규칙**: 테이블 단위 `std::shared_mutex`를 거치지 않고 테이블 데이터·인덱스를 읽거나 쓰지 않는다. id 발급 → 파일 append → 인덱스 갱신은 하나의 배타 락 임계구역에서 수행한다. 락을 잡은 채 네트워크 I/O 금지.
4. **id는 자동 부여만 허용**: 사용자가 INSERT로 id를 지정하면 에러로 거부한다. id는 단조 증가, 재사용 금지.
5. **데이터 무결성**: `data/` 아래 사용자 데이터 파일을 테스트가 덮어쓰거나 삭제하지 않는다. 테스트는 임시 디렉터리(`/tmp` 하위가 아닌 테스트별 고유 경로)에서만 실행하고 끝나면 정리한다.
6. **성능 수치 조작 금지**: 벤치마크는 실제 측정값만 문서에 기록한다. 측정하지 않은 수치를 README에 쓰지 않는다.
7. **예외로 흐름 제어 금지**: 에러는 `Result<T>`로 반환한다. 서버 워커 스레드에서 예외가 새어 나가 프로세스가 죽는 일이 없어야 한다.
8. **요청 없이 커밋/푸시 금지**. 파괴적 명령(`rm -rf`, `git reset --hard` 등)은 대상 확인 후에만.
9. 테스트가 실패한 채로 완료라고 보고하지 않는다.

## 아키텍처

### 기술 스택
- C++17, clang++/g++, **Makefile** 빌드 (이 환경에 cmake 없음), POSIX 소켓, `std::thread`/`std::shared_mutex`
- 테스트: 자체 경량 하네스 `tests/test_util.h` (GoogleTest 미사용)
- 데이터: 테이블별 `schema.json` + append-only 바이너리 힙 파일

### 폴더 구조
```
mini-dbms/
├── CLAUDE.md
├── Makefile
├── README.md                # 사용법, 설계, 벤치마크 결과
├── src/
│   ├── common/              # Result<T>, Value/타입, 유틸
│   ├── sql/                 # lexer, parser, AST
│   ├── storage/             # 스키마 로더, 힙 파일(data.bin), 레코드 직렬화
│   ├── index/               # B+ 트리 (key=id → 파일 offset)
│   ├── engine/              # Engine, Table(락 포함), Executor, 실행 계획
│   ├── server/              # ThreadPool, HTTP 파서, 서버, JSON 출력
│   └── cli/main.cpp         # minidb CLI
├── tests/                   # 단위/기능/동시성 테스트
├── bench/                   # 100만 건 생성 + 인덱스 vs 선형 탐색 벤치마크
├── data/                    # 예시 테이블 (users/schema.json 등)
└── docs/                    # 설계 설명, benchmark.md
```

### 데이터 흐름
`SQL 문자열 → Lexer → Parser(AST) → Executor → Table(shared_mutex) → Storage + BPlusTree`

### 핵심 설계
- id 조건(`=`, `BETWEEN`, `>`, `<`)은 B+ 트리, 그 외 컬럼은 선형 스캔. 결과에 `plan: INDEX|SCAN` 표기.
- 재시작 시 `data.bin` 순차 스캔으로 인덱스 재구축, `next_id` 복구.
- 서버: acceptor → 작업 큐(조건변수) → 고정 크기 ThreadPool. 큐 포화 시 503.

## 빌드 / 테스트 / 실행

```bash
make                    # 전체 빌드 (build/ 생성): minidb, minidb_server, bench, tests
make test               # 단위 + 기능 + 동시성 테스트
make tsan               # ThreadSanitizer 빌드로 동시성 테스트 실행
make bench              # 100만 건 벤치마크 → docs/benchmark.md 갱신
make clean

./build/minidb --data-dir data -e "SELECT * FROM users WHERE id = 1;"
./build/minidb --data-dir data --file query.sql
./build/minidb_server --data-dir data --port 8080 --threads 8
curl -X POST localhost:8080/query -d "SELECT * FROM users WHERE id = 1"
curl localhost:8080/health
```

## 코딩 컨벤션

### 네이밍
- 타입/클래스: `PascalCase` (`BPlusTree`, `ThreadPool`)
- 함수/변수: `snake_case` (`insert_row`), 멤버 변수: 접미사 `_` (`mutex_`)
- 상수/enum 값: `kPascalCase` (`kMaxVarcharLen`)
- 파일: `snake_case.h/.cpp`, 헤더 가드는 `#pragma once`
- 네임스페이스: `minidb::<module>`

### 코드 패턴
- RAII 필수: 락은 `std::lock_guard`/`std::shared_lock`/`std::unique_lock`, 소유권은 `std::unique_ptr`. 원시 `new/delete` 지양.
- 에러 반환은 `Result<T>`, 사용자 입력 오류 메시지는 한국어가 아닌 영어 (CLI/JSON 일관성).
- 함수는 한 가지 일만, 헤더에는 선언과 짧은 주석(왜 그런지)만. 핵심 알고리즘(B+ 트리 분할, 락 순서)은 주석으로 근거를 남긴다.
- 모듈 간 의존 방향: `common ← sql, storage, index ← engine ← server, cli`. 역방향 include 금지.
- 모든 새 기능은 테스트와 함께 추가한다 (엣지 케이스 포함).

### 커밋
- 형식: `<type>(<scope>): <요약>` — type: `feat|fix|test|docs|refactor|perf|build|chore`, scope: `sql|storage|index|engine|server|cli|bench`
- 예: `feat(index): B+ 트리 리프 분할 구현`
- 커밋은 Phase/기능 단위로 작게, 테스트가 통과한 상태에서만. 사용자가 요청할 때만 커밋한다.
- 현재 git 미초기화 상태 — 커밋 전 사용자 확인 후 `git init`.
