# Contexto do estudo

## Projeto

O repositório contém fontes históricos do InterBase. O foco deste estudo é entender o entry point Linux e a arquitetura de execução concorrente do servidor.

Arquivos de estudo formatados, sem alterar os originais:

- `.work/remote/inet_server.c`
- `.work/remote/server.c`

Os arquivos em `.work/` são ignorados pelo Git. Os originais continuam em:

- `remote/inet_server.c`
- `remote/server.c`

## Entry point Linux

O entry point está em `remote/inet_server.c`, na função `main()` original, aproximadamente nas linhas 144–433.

Fluxo principal:

1. `main()` inicializa flags e analisa argumentos.
2. Com `SUPERSERVER`, define `multi_client`, `multi_threaded` e `standalone` como `TRUE`.
3. Obtém uma `PORT` por `INET_connect()` ou `INET_server()`.
4. Se `multi_threaded` for verdadeiro, chama `SRVR_multi_thread()`.
5. Caso contrário, chama `SRVR_main()`.

Trecho decisivo em `remote/inet_server.c`:

```c
if (multi_threaded)
    SRVR_multi_thread(port, INET_SERVER_flag);
else
    SRVR_main(port, INET_SERVER_flag);
```

No build Linux, o destino tradicional do servidor de rede é `gds_inet_server`, conforme `builds/original/prefix.linux` e `builds/original/inetd.conf.unx`.

## Inicialização dos recursos de comunicação

`main()` coordena a inicialização, mas a criação/configuração dos recursos de rede fica nas rotinas `INET_connect()` e `INET_server()`:

- no modo standalone/SuperServer, `main()` chama `INET_connect()` sem pacote; essa função aloca a `PORT`, cria o socket, faz `bind()` e `listen()`;
- no modo iniciado por um serviço externo (como `inetd`), `main()` chama `INET_server(channel)`; o socket já foi estabelecido pelo processo externo, e a função apenas cria a `PORT` e associa o descritor recebido;
- depois disso, `main()` entrega a porta para `SRVR_multi_thread()` ou `SRVR_main()`, que inicializa a camada do servidor e processa as requisições.

Assim, `main()` inicia o fluxo e escolhe o modo, `INET_*` prepara a comunicação, e `SRVR_*` usa essa comunicação.

## `inetd` e sockets de conexão

No modo `inetd`, o gerenciador de serviços Unix mantém a porta do serviço em escuta, aceita a conexão e inicia `gds_inet_server` entregando o socket já conectado ao processo. A configuração histórica está em `builds/original/inetd.conf.unx`:

```text
gds_db stream tcp nowait root /usr/interbase/bin/gds_inet_server gds_inet_server
```

Nesse caminho, `INET_server(channel)` apenas cria a `PORT` e associa o descritor recebido. No modo standalone/SuperServer, `INET_connect()` cria o socket, faz `bind()` e `listen()`.

Um socket TCP é um endpoint de comunicação identificado, em uma conexão, pelo conjunto:

```text
IP_cliente:porta_cliente ↔ IP_servidor:porta_servidor
```

O socket listener fica associado à porta do serviço, como `3050`. Quando ocorre `accept()`, o sistema cria um socket conectado específico para o cliente, enquanto o listener continua aceitando novas conexões. Portanto, várias conexões podem usar a mesma porta local do servidor; elas são diferenciadas pelo IP e pela porta de origem de cada cliente.

`main_port` é a `PORT` principal recebida por `SRVR_multi_thread()`. No SuperServer multi-clientes, ela representa a porta-mãe/listener. `RECEIVE(main_port, ...)` monitora essa porta e, ao detectar uma nova conexão, `accept()` cria uma nova `PORT` para o cliente. A `main_port` não é a conexão individual de cada cliente.

### O que `set_server()` procura

O “servidor” de `set_server()` não é uma conexão TCP, um processo novo ou uma thread nova. A função percorre a lista global `servers` e compara `port->port_type` com `server->srvr_port_type` — por exemplo, o tipo `port_inet`. Se não houver uma estrutura lógica `SRVR` para aquele tipo de transporte, aloca uma nova estrutura, registra `srvr_parent_port` e as flags, e associa o resultado em `port->port_server`.

Assim, `set_server()` reutiliza ou cria um contexto lógico do servidor por tipo de transporte. A conexão específica do cliente já é representada por uma `PORT` própria, criada no `accept()`; ela não é localizada por seu par `IP:porta` em `set_server()`. Também não há criação de socket, processo ou thread nessa função.

O objetivo prático de `port->port_server` é permitir que o restante do servidor trate a `PORT` sabendo a que grupo lógico ela pertence e qual é sua porta-pai. O dispatcher usa `srvr_parent_port` para distinguir a porta principal das portas de clientes, localizar a conexão correta na lista `port_clients` e aplicar corretamente operações como processamento e encerramento.

`main_port` tem, sim, um tipo: `port->port_type`. Para uma conexão TCP/IP, `INET` define esse campo como `port_inet`. Esse tipo indica o transporte e permite selecionar/comparar a implementação compatível — por exemplo, INET, XNET, IPC, DECnet ou named pipe. Não é o tipo do cliente nem uma identificação de `IP:porta`; é uma classificação do mecanismo de comunicação. Em geral, `set_server()` mantém um `SRVR` lógico por valor de `port_type` dentro daquele processo.

Se um único processo inicializasse efetivamente TCP/IP e IPC, sua lista `servers` teria duas entradas: uma para `port_inet` e outra para `port_ipc`. Neste código histórico, contudo, os entry points aparecem separados: `inet_server.c` conduz o servidor INET/TCP, enquanto `ipc_server.c` conduz o servidor IPC. Como `servers` é uma variável global do processo, não se deve somar automaticamente os `SRVR`s dos dois executáveis como se fossem uma única lista; cada processo cria suas próprias entradas conforme os transportes que inicializa.

Essa separação no Linux é uma decisão da organização histórica dos executáveis, não uma obrigação do sistema operacional. O código Windows possui entry points/rotinas que podem reunir mais de um transporte no mesmo processo; nesse desenho, se TCP/IP e IPC criarem `PORT`s no mesmo processo, a lista poderá conter os dois `SRVR`s. Um servidor Linux também poderia ser reestruturado para fazer isso, desde que tivesse um dispatcher capaz de monitorar os dois mecanismos.

Síntese por plataforma neste código histórico: no Windows, um processo/entry point pode reunir transportes diferentes e usar threads para o dispatcher e/ou atendimento das portas; no Linux/Unix, a organização tradicional separa os entry points por transporte, como `gds_inet_server` para INET/TCP e `ipc_server` para IPC. Essa separação é por tipo de transporte/servidor, não necessariamente um processo por conexão: as conexões podem ser multiplexadas por um servidor multi-clientes, atendidas por threads ou, em configurações Classic/`inetd`, por processos separados conforme o modo de execução.

Não há ganho automático de desempenho em dividir um serviço em várias portas. Uma única porta pode multiplexar muitas conexões; a separação em portas diferentes é útil principalmente para isolamento, regras de firewall, protocolos distintos ou arquiteturas específicas de balanceamento.

A porta usada pelo cliente é normalmente efêmera: escolhida automaticamente pelo sistema operacional para uma conexão de saída e liberada depois. Firewalls com estado normalmente precisam apenas permitir a saída do cliente para a porta conhecida do servidor e a entrada do servidor nessa porta; o tráfego de retorno para a porta efêmera é permitido por pertencer à conexão já estabelecida. Firewalls stateless ou políticas de saída muito restritivas podem exigir regras adicionais.

## SUPERSERVER, MULTI_THREAD e multi_threaded

São conceitos diferentes:

- `SUPERSERVER`: definição de compilação que seleciona a arquitetura SuperServer.
- `MULTI_THREAD`: suporte de threading habilitado no build.
- `multi_threaded`: variável em tempo de execução usada para escolher `SRVR_multi_thread()` ou `SRVR_main()`.

No Linux, `jrd/thd.h` define `MULTI_THREAD` quando `LINUX` e `SUPERSERVER` estão definidos.

Em `remote/inet_server.c`, o bloco `#ifdef SUPERSERVER` faz:

```c
INET_SERVER_flag |= SRVR_multi_client;
multi_client = multi_threaded = standalone = TRUE;
```

O `if (multi_threaded)` continua presente porque o mesmo fonte suporta outras configurações. Em um build Linux `SUPERSERVER`, ele será verdadeiro.

Resumo prático do InterBase histórico:

- Classic: processos separados por conexão.
- SuperServer: um processo compartilhado com várias threads.

## THREAD_ENTER e THREAD_EXIT

Essas macros não criam threads. Elas registram a thread atual no mecanismo interno de sincronização/scheduler do InterBase.

Em `jrd/thd.h`:

```c
#ifdef MULTI_THREAD
  #ifdef SUPERSERVER
    #define THREAD_ENTER SCH_enter()
    #define THREAD_EXIT  SCH_exit()
  #else
    #define THREAD_ENTER gds__thread_enter()
    #define THREAD_EXIT  gds__thread_exit()
  #endif
#endif
```

`gds__thread_enter()` também chama `SCH_enter()`. Se o suporte de threading não estiver compilado, as macros podem ser vazias.

O scheduler interno não substitui o scheduler do Linux/Windows. O sistema operacional cria e agenda as threads; o InterBase controla a entrada delas na camada de acesso ao banco, usando lista circular, mutexes, eventos e filas.

## Inicialização do scheduler

`SRVR_multi_thread()` não assume cegamente que tudo já foi inicializado. No começo da função (`remote/server.c`, linhas 254–259), há:

```c
gds__thread_enable(-1);
ISC_event_init(thread_event, 0, 0);
THREAD_ENTER;
SET_THREAD_DATA;
trdb->trdb_setjmp = env;
trdb->trdb_status_vector = status_vector;
```

`gds__thread_enable(-1)`:

- habilita o sistema de threads;
- chama `SCH_init()`;
- inicializa o suporte de threads (`THD_INIT`).

`SCH_enter()` registra a thread na lista circular de threads ativas. `SET_THREAD_DATA` associa um contexto específico à thread; ele não chama `setjmp`.

## SRVR_multi_thread()

A função está em `remote/server.c`, originalmente nas linhas 221–513.

Ela funciona como dispatcher:

1. inicializa o scheduler e o contexto da thread;
2. chama `set_server()`;
3. recebe requisições por `RECEIVE(main_port, ...)`;
4. coloca requisições em filas;
5. acorda ou cria threads de trabalho;
6. usa `ISC_event_post(thread_event)` para sinalizar trabalho disponível.

A criação efetiva de uma thread ocorre em torno da linha 495:

```c
gds__thread_start(
    (FPTR_INT) thread,
    (void*) flags,
    THREAD_medium,
    THREAD_ast,
    NULL_PTR);
```

No Linux, `gds__thread_start()` chega a `pthread_create()` e `pthread_detach()` em `jrd/thd.c`.

A função `thread()` também executa `THREAD_ENTER` e espera por trabalho. Portanto, a thread de comunicação/dispatcher e as threads de trabalho participam do scheduler quando entram na camada controlada pelo InterBase.

O Linux ainda pode interromper qualquer thread. A lista circular do InterBase não impede preempção do SO; ela coordena o acesso interno ao banco.

## setjmp/longjmp

Em Linux, `jrd/ibsetjmp.h` define:

```c
#define SETJMP setjmp
#define LONGJMP longjmp
#define JMP_BUF jmp_buf
```

`setjmp()` salva um contexto de execução. Na primeira passagem retorna `0`. Se uma função profunda chamar `longjmp()`, a execução volta ao `setjmp()` salvo, que então retorna um valor diferente de zero.

Não é `assert()` e não é um monitor assíncrono. É um checkpoint de recuperação, semelhante a um `goto` não local, porém restaurando também o contexto da pilha.

Em `SRVR_multi_thread()`:

- o primeiro `SETJMP(env)` protege a inicialização;
- o segundo `SETJMP(env)` protege o loop principal;
- o segundo tratador normalmente limpa `port`/`request` e continua no `while (TRUE)`;
- em erros irrecuperáveis, como falha de `RECEIVE(main_port, ...)`, a função faz `return`.

`SET_THREAD_DATA` apenas configura `trdb` e chama `THD_put_specific()`. Não estabelece outro `setjmp`.

O destino de `LONGJMP` é controlado por `trdb->trdb_setjmp`. Durante uma recuperação aninhada, ele aponta temporariamente para `inner_env` e depois volta a apontar para `env`.

## Comparações de arquiteturas

### Informix

Informix mantém a abstração de Virtual Processors (VPs). Um VP é uma unidade de execução que agenda internamente threads lógicas do banco. Há classes como CPU VP, AIO VP e SHM/network VP.

### Db2

Db2 usa EDUs (Engine Dispatchable Units), como agents, prefetchers, loggers e page cleaners. As EDUs são implementadas como threads do engine. Elas são mais próximas das threads de trabalho do InterBase do que dos VPs do Informix.

### SQL Server

SQL Server usa threads do SO organizadas pelo SQLOS:

```text
SQLOS scheduler -> workers -> tasks
```

É uma arquitetura híbrida: as threads são reais, mas existe um scheduler interno que organiza workers e tasks. Conceitualmente, fica entre o modelo direto do Db2/InterBase e a camada de VPs do Informix.

### Oracle

Oracle oferece:

- Dedicated Server: um processo servidor por cliente;
- Shared Server: dispatchers recebem requisições e as colocam em filas para shared servers;
- modos de execução threaded em determinadas plataformas/configurações.

O Shared Server é conceitualmente semelhante a um dispatcher com fila e pool de workers.

### MySQL

O modelo padrão é uma thread do SO por conexão. O MySQL Enterprise oferece Thread Pool, que separa conexões de threads executoras e usa grupos de threads.

### PostgreSQL

O modelo normal é processo backend por conexão. O supervisor cria um processo separado para cada cliente. Consultas paralelas também usam processos workers adicionais. PostgreSQL não tem um modelo geral de threads de sessão equivalente ao SuperServer do InterBase.

### Firebird

Firebird preserva três modos explícitos:

- Classic / MultiProcess: processo por conexão, cache próprio;
- SuperClassic / ThreadedShared: um processo com threads, cache de páginas próprio por conexão;
- SuperServer / ThreadedDedicated: um processo com threads e cache compartilhado.

Firebird é diretamente comparável ao InterBase histórico.

## Cache de páginas / buffer pool

“Cache separado” significa o cache de páginas de dados do engine, não o cache de arquivos do sistema operacional.

Processos separados não obrigam caches separados. Oracle e PostgreSQL usam processos separados, mas compartilham memória para estruturas como a SGA/buffer cache do Oracle e `shared_buffers` do PostgreSQL.

No Firebird e no InterBase histórico, a escolha depende da arquitetura:

- Classic: caches separados por processo/conexão;
- SuperClassic: caches separados por conexão, apesar de um processo compartilhado;
- SuperServer: cache compartilhado.

Caches separados não implicam corrupção. A consistência é obtida por locking, memória compartilhada para coordenação e controle transacional/MVCC. O custo é possível duplicação de páginas em memória e leituras repetidas.

## Modelo mental final

```text
Sistema operacional:
  cria e agenda processos/threads reais

Engine do banco:
  coordena sessões, filas, locks, eventos e cache

Informix:
  VP é uma camada de execução e scheduler

Db2:
  EDU é uma unidade/thread de trabalho

SQL Server:
  SQLOS scheduler organiza workers/tasks

InterBase/Firebird SuperServer:
  processo compartilhado + threads + scheduler interno + cache compartilhado

PostgreSQL/InterBase Classic/Firebird Classic:
  processos separados por conexão, com coordenação entre processos
```
