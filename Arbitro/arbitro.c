// Arbitro.c (Servidor que recebe múltiplas mensagens/clientes e encerra com "sair")
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h> // Incluir stdbool.h para bool

// Usar um nome de pipe consistente! Exemplo:
#define PIPE_NAME _T("\\\\.\\pipe\\pipe")
#define BUFFER_SIZE 512

// Função para criar o pipe (DUPLEX)
HANDLE CriarPipeServidorDuplex() {
    HANDLE hPipe = CreateNamedPipe(
        PIPE_NAME, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, BUFFER_SIZE, BUFFER_SIZE, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        _tprintf(TEXT("Erro ao criar o pipe servidor duplex: %lu\n"), GetLastError());
    }
    else {
        _tprintf(TEXT("Pipe servidor '%s' criado. Aguardando clientes...\n"), PIPE_NAME);
        }
    return hPipe;
}


int _tmain(int argc, LPTSTR argv[]) {

#ifdef UNICODE
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    HANDLE hPipe = CriarPipeServidorDuplex();
    if (hPipe == INVALID_HANDLE_VALUE) {
        return -1;
    }

    TCHAR chBuf[BUFFER_SIZE]; // Renomeado de buffer para clareza
    DWORD cbRead; // Renomeado de dwWritten para clareza na leitura
    BOOL fSuccess;
    BOOL fKeepServerRunning = TRUE; // Flag para controlar o loop principal

    // *** LOOP EXTERNO PARA ACEITAR CONEXÕES ***
    while (fKeepServerRunning) {
        _tprintf(TEXT("\nAguardando conexão do cliente...\n"));

        // Esperar cliente conectar
        BOOL fConnected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (fConnected) {
            _tprintf(TEXT("Cliente conectado. Aguardando mensagens...\n"));

            // *** LOOP INTERNO PARA LER MENSAGENS DESTE CLIENTE ***
            do {
                // Ler dados do cliente
                fSuccess = ReadFile(
                    hPipe,              // handle do pipe
                    chBuf,              // buffer para receber dados
                    BUFFER_SIZE * sizeof(TCHAR), // tamanho MÁXIMO do buffer em BYTES
                    &cbRead,            // bytes lidos
                    NULL);              // síncrono

                // Verificar erros na leitura
                if (!fSuccess && GetLastError() != ERROR_MORE_DATA) {
                    DWORD dwError = GetLastError();
                    if (dwError == ERROR_BROKEN_PIPE) {
                        _tprintf(TEXT("Cliente desconectou (ERROR_BROKEN_PIPE).\n"));
                    }
                    else {
                        _tprintf(TEXT("ReadFile falhou, GLE=%lu.\n"), dwError);
                    }
                    break; // Sair do loop INTERNO (deste cliente)
                }

                // Processar dados lidos (SE houver)
                if (cbRead > 0) {
                    // *** ADICIONAR TERMINADOR NULO - ESSENCIAL ***
                    DWORD numChars = cbRead / sizeof(TCHAR);
                    if (numChars < BUFFER_SIZE) {
                        chBuf[numChars] = _T('\0');
                    }
                    else {
                        chBuf[BUFFER_SIZE - 1] = _T('\0'); // Truncar
                        _tprintf(TEXT("Aviso: Mensagem pode ter sido truncada.\n"));
                    }

                    // Mostrar a mensagem recebida
                    _tprintf(TEXT("Recebido: \"%s\"\n"), chBuf);

                    // *** VERIFICAR COMANDO DE SAÍDA ***
                    if (_tcsicmp(chBuf, _T("sair")) == 0) {
                        _tprintf(TEXT("Comando 'sair' recebido. A terminar servidor...\n"));
                        fKeepServerRunning = FALSE; // Sinaliza para sair do loop EXTERNO
                        // fSuccess = FALSE; // Opcional: Força saída do loop interno também
                        break; // Sai do loop de leitura deste cliente
                    }
                    // --- Processar outros comandos aqui ---

                }
                else if (fSuccess && cbRead == 0) { // Leitura 0 bytes com sucesso?
                    _tprintf(TEXT("Cliente desconectou (leitura 0 bytes)?\n"));
                    break; // Sair do loop interno
                }

                // Continuar loop INTERNO enquanto sucesso ou mais dados
            } while (fSuccess || (!fSuccess && GetLastError() == ERROR_MORE_DATA));

            _tprintf(TEXT("Fim da comunicação com este cliente.\n"));

            // *** DESCONECTAR O CLIENTE ATUAL ANTES DE ESPERAR O PRÓXIMO ***
            DisconnectNamedPipe(hPipe);
            _tprintf(TEXT("Cliente desconectado. Pronto para o próximo.\n"));

        }
        else { // fConnected foi falso
            _tprintf(TEXT("Erro ao esperar conexão do cliente (ConnectNamedPipe), GLE=%lu.\n"), GetLastError());
            Sleep(500); // Pequena pausa antes de tentar novamente
        }

        // Se fKeepServerRunning se tornou false (por causa do comando "sair"),
        // o loop while externo terminará aqui.

    } // Fim do loop while(fKeepServerRunning)

    // Limpeza final
    _tprintf(TEXT("Servidor Arbitro a terminar...\n"));
    CloseHandle(hPipe);
    return 0;
}