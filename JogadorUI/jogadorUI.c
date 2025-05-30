#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h> 
#include <time.h> 
#include <stdbool.h>
#include <strsafe.h>
#include "mp.h"

//#define DEBUG_MODE 

#ifdef DEBUG_MODE
    #define LOG_DEBUG(format, ...) _tprintf(_T("[DEBUG] :%d: " format _T("\n")), __LINE__, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(FORMAT, ...)
#endif


#define PIPE_NAME _T("\\\\.\\pipe\\pipe")


#define MAX_CONSOLE_EVENTS 128
#define INPUT_BUFFER_SIZE 512
#define BUFFER_SIZE 512

#define LINHA_LETRAS_ALEATORIAS 0 // Linha onde as letras aleatórias serão impressas

#define NAME_SIZE 20
#define LINHA_INPUT_CLIENTE 2 // Linha onde o input do cliente será lido
#define PROMPT_INPUT _T("input> ") // Prompt para o input do cliente

#define LINHA_MENSAGENS_SERVIDOR 4 // Linha onde as mensagens do servidor serão impressas
#define COMANDO_DESLIGAR_CLIENTE _T("/sair") // Comando para desligar o cliente


#define NOME_MUTEX_CONSOLE NULL


volatile bool run = true;

typedef struct {
	// memoria partilhada
    MP* dados;
    HANDLE hMutex;
    HANDLE hEvento;

	HANDLE mapFile;

    // pipe
    HANDLE hpipe;
    TCHAR nomeJogador[NAME_SIZE];

	// console
	HANDLE hStdin;        // Handle para o INPUT da consola
	DWORD originalStdInMode; // Para guardar o modo original do stdin
	HANDLE hStdout;
	CONSOLE_CURSOR_INFO originalCursorInfo;
	HANDLE hMutexConsole; // Mutex para proteger a consola 
} globais;

void limparLinha(SHORT y, HANDLE hStdout) {
    COORD coord = { 0, y };

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD charsWritten;
    if (!GetConsoleScreenBufferInfo(hStdout, &csbi)) return;

    SetConsoleCursorPosition(hStdout, coord);
    FillConsoleOutputCharacter(hStdout, _T(' '), csbi.dwSize.X, coord, &charsWritten);
    SetConsoleCursorPosition(hStdout, coord);
}

void moverCursor(SHORT x, SHORT y, HANDLE hStdout) {
    COORD coord = { x, y };
    SetConsoleCursorPosition(hStdout, coord);
}

void imprimirVetor(TCHAR* letras, HANDLE hStdout) {
    COORD coord = { 0, LINHA_LETRAS_ALEATORIAS };
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (!GetConsoleScreenBufferInfo(hStdout, &csbi)) return;
    SetConsoleCursorPosition(hStdout, coord);

    int charsWritten = 0;
    for (int i = 0; i < MAXLETRAS; i++) {
        TCHAR charToPrint = (letras[i] != _T('\0')) ? letras[i] : _T('_');
        _puttchar(charToPrint);
        _puttchar(_T(' '));
        charsWritten += 2; // Conta letra + espaço
    }

    // Limpa o resto da linha caso MAXLETRAS mude (ou para garantir)
    DWORD dwCharsWritten;
    FillConsoleOutputCharacter(hStdout, _T(' '), csbi.dwSize.X - charsWritten, // Nº de espaços a preencher
        (COORD) {
        (SHORT)charsWritten, coord.Y
    }, // Posição onde começar a preencher
        & dwCharsWritten);

    fflush(stdout);
}

DWORD WINAPI threadLetrasOutput(LPVOID lpParam) {
    globais* g = (globais*)lpParam;

    while (run) {

        if (!run) break;

        if (WaitForSingleObject(g->hEvento, 500) == WAIT_OBJECT_0) {
            if (WaitForSingleObject(g->hMutex, INFINITE) == WAIT_OBJECT_0) {

                TCHAR letrasCopia[MAXLETRAS];
                memcpy(letrasCopia, g->dados->letras, MAXLETRAS * sizeof(TCHAR));

                ReleaseMutex(g->hMutex);

                if (WaitForSingleObject(g->hMutexConsole, INFINITE) == WAIT_OBJECT_0) {
                    imprimirVetor(letrasCopia, g->hStdout);
                    ReleaseMutex(g->hMutexConsole);
                }
            }
        }
    }
    return 0;
}

BOOL createPipe(globais* g) {
    if (g == NULL) return FALSE;

    g->hpipe = CreateFile(
        PIPE_NAME,                    // Nome do pipe
        GENERIC_READ | GENERIC_WRITE, // Acesso para ler e escrever
		0,                            // dwShareMode: sem compartilhamento
        NULL,                         // lpSecurityAttributes: segurança default
        OPEN_EXISTING,                // Abrir apenas se o pipe já existir (criado pelo servidor)
        0,                            // dwFlagsAndAttributes: I/O síncrono padrão
        NULL);                        // hTemplateFile: não usado para pipes

    if (g->hpipe == INVALID_HANDLE_VALUE) {
        LOG_DEBUG(_T("createPipe: ERRO ao conectar ao pipe '%s'. Código GetLastError(): %lu\n"),
            PIPE_NAME, GetLastError());
        g->hpipe = NULL;
        return FALSE;
    }

    return TRUE;
}

BOOL enviarMensagemPipe(HANDLE hpipe, const TCHAR* mensagem) {
    if (hpipe == NULL || hpipe == INVALID_HANDLE_VALUE || mensagem == NULL) return FALSE;

    DWORD bytesOutput = (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR);
    DWORD bytesWritten = 0;

    BOOL result = WriteFile(hpipe, mensagem, bytesOutput, &bytesWritten, NULL);

    if (!result) 
        LOG_DEBUG(_T("CLIENTE ERRO (enviarComando): Falha ao enviar mensagem '%s' pelo pipe (%lu).\n"), mensagem, GetLastError());
    else if (bytesWritten != bytesOutput) {
        LOG_DEBUG(_T("CLIENTE AVISO (enviarComando): Nem todos os bytes foram enviados para '%s'. (Esperado: %lu, Escrito: %lu)\n"), mensagem, bytesOutput, bytesWritten);
		//pensar nessa logica, talvez nao mandar aviso se o servidor nao espera por todos os bytes ou se o pipe nao estiver configurado para esperar por todos os bytes
        return FALSE; 
    }
    return result;
}

DWORD WINAPI threadEscutarInput(LPVOID lpParam) {
    globais* g = (globais*)lpParam;
	LOG_DEBUG(_T("threadEscutarInput: Iniciando threadEscutarInput...\n"));

	INPUT_RECORD inputRecord[MAX_CONSOLE_EVENTS]; // Buffer para eventos de input
    DWORD numEventsRead;
	TCHAR userInputBuffer[INPUT_BUFFER_SIZE];
	DWORD userInputLen = 0; 

    TCHAR comandoParaEnviar[INPUT_BUFFER_SIZE];
    BOOL deveEnviarComando = FALSE;

    TCHAR prompt[] = PROMPT_INPUT;
    DWORD promptLen = (DWORD)_tcslen(prompt);
    DWORD lenWritten;

    COORD promptCoord = { 0, LINHA_INPUT_CLIENTE };
    SHORT inputStartX = (SHORT)promptLen;

    // Escrever prompt inicial (protegido)
    if (WaitForSingleObject(g->hMutexConsole, INFINITE) == WAIT_OBJECT_0) {
        limparLinha(LINHA_INPUT_CLIENTE, g->hStdout);
        WriteConsoleOutputCharacter(g->hStdout, prompt, promptLen, promptCoord, &lenWritten);
        moverCursor(inputStartX, LINHA_INPUT_CLIENTE, g->hStdout);
        ReleaseMutex(g->hMutexConsole);
    }

    while (run) {
        deveEnviarComando = FALSE; // Resetar flag

        if (!ReadConsoleInput(g->hStdin, inputRecord, MAX_CONSOLE_EVENTS, &numEventsRead)) {
            LOG_DEBUG(_T("threadEscutarInput: Erro ReadConsoleInput (%lu)\n"), GetLastError());
            run = false;
            continue;
        }

        // Processar cada evento lido
        for (DWORD i = 0; i < numEventsRead; ++i) {
            if (inputRecord[i].EventType == KEY_EVENT && 
                inputRecord[i].Event.KeyEvent.bKeyDown) 
            {
                if (WaitForSingleObject(g->hMutexConsole, INFINITE) == WAIT_OBJECT_0) {
					LOG_DEBUG(_T("threadEscutarInput: Evento de tecla pressionada detectado.\n"));
                    TCHAR ch = inputRecord[i].Event.KeyEvent.uChar.UnicodeChar;
                    WORD vkCode = inputRecord[i].Event.KeyEvent.wVirtualKeyCode;

					if (vkCode == VK_RETURN) { // Tecla Enter
						LOG_DEBUG(_T("threadEscutarInput: Tecla Enter pressionada.\n"));
                        userInputBuffer[userInputLen] = _T('\0');

                        if (userInputLen > 0) {
                            _tcscpy_s(comandoParaEnviar, INPUT_BUFFER_SIZE, userInputBuffer);
                            deveEnviarComando = TRUE;
                        }

                        userInputLen = 0;
                        userInputBuffer[0] = _T('\0');
                        moverCursor(0, LINHA_INPUT_CLIENTE, g->hStdout);
                        limparLinha(LINHA_INPUT_CLIENTE, g->hStdout);

                        WriteConsoleOutputCharacter(g->hStdout, prompt, promptLen, promptCoord, &lenWritten);
                        moverCursor(inputStartX, LINHA_INPUT_CLIENTE, g->hStdout);
                    }
					else if (vkCode == VK_BACK) { // Tecla Backspace
                        if (userInputLen > 0) {
                            userInputLen--;
                            SHORT currentX = inputStartX + (SHORT)userInputLen;
                            COORD backspaceCoord = { currentX, LINHA_INPUT_CLIENTE };
                            
                            WriteConsoleOutputCharacter(g->hStdout, _T(" "), 1, backspaceCoord, &lenWritten);
                            moverCursor(currentX, LINHA_INPUT_CLIENTE, g->hStdout);

                            userInputBuffer[userInputLen] = _T('\0');
                        }
                    }
					else if (ch >= _T(' ') && userInputLen < INPUT_BUFFER_SIZE - 1) {
                        userInputBuffer[userInputLen] = ch;

                        SHORT currentX = inputStartX + (SHORT)userInputLen;
                        COORD charCoord = { currentX, LINHA_INPUT_CLIENTE };
                        
                        WriteConsoleOutputCharacter(g->hStdout, &ch, 1, charCoord, &lenWritten);
                        moverCursor(currentX + 1, LINHA_INPUT_CLIENTE, g->hStdout);

                        userInputLen++;
                    }
                    ReleaseMutex(g->hMutexConsole);
                }

                if (deveEnviarComando) {
                        if (!enviarMensagemPipe(g->hpipe, comandoParaEnviar)) {
							LOG_DEBUG(_T("threadEscutarInput: Falha ao enviar comando '%s' pelo pipe.\n"), comandoParaEnviar);
                            run = false;
                        }
                    comandoParaEnviar[0] = _T('\0');
                    deveEnviarComando = FALSE;
                }

            } // Fim if (evento de tecla)
        } // Fim for (eventos)
    } // Fim while(run)

    if (WaitForSingleObject(g->hMutexConsole, INFINITE) == WAIT_OBJECT_0) {
        limparLinha(LINHA_INPUT_CLIENTE, g->hStdout);
        ReleaseMutex(g->hMutexConsole);
    }
    return 0;
}

DWORD WINAPI receberComandosPipe(LPVOID lpParam) {
    globais* g = (globais*)lpParam;
	LOG_DEBUG(_T("receberComandosPipe: Iniciando thread receberComandosPipe...\n"));

    TCHAR buffer[BUFFER_SIZE];
    HANDLE hMutexConsole = g->hMutexConsole;

    if (g == NULL || g->hpipe == NULL || g->hpipe == INVALID_HANDLE_VALUE) return 1;
    
    while (run) {
        if (!run) break;

        DWORD bytesDisponiveis = 0;
        DWORD msgBytes = 0;

        // Espreitar o pipe
        BOOL peekResult = PeekNamedPipe(
            g->hpipe,
            NULL,               // buffer (não queremos ler dados aqui)
            0,                  // buffer size
            NULL,               // bytes peeked
            &bytesDisponiveis,  // total bytes available
            &msgBytes           // bytes in next message
        );

        if (!run) break;
        if (!peekResult) {run = false; continue; } // Assume que a conexão foi perdida

        if (msgBytes > 0) {
			LOG_DEBUG(_T("receberComandosPipe: Dados disponíveis no pipe. Bytes disponíveis: %lu, Bytes na próxima mensagem: %lu\n"), bytesDisponiveis, msgBytes);
            DWORD bytesRead = 0;
            
            BOOL mensagemServidor = ReadFile(
                g->hpipe,
                buffer,
                (sizeof(buffer) / sizeof(TCHAR) - 1) * sizeof(TCHAR),
                &bytesRead,
                NULL);

            if (!run) break;

            if (mensagemServidor && bytesRead > 0) {
				LOG_DEBUG(_T("receberComandosPipe: Mensagem recebida do servidor. Bytes lidos: %lu\n"), bytesRead);
                buffer[bytesRead / sizeof(TCHAR)] = _T('\0');
                
                if (hMutexConsole && WaitForSingleObject(hMutexConsole, INFINITE) == WAIT_OBJECT_0) {
                    for (SHORT i = 0; i < 50; ++i)
                        limparLinha(LINHA_MENSAGENS_SERVIDOR + i, g->hStdout);

                    moverCursor(0, LINHA_MENSAGENS_SERVIDOR, g->hStdout);
                    _tprintf(_T("Servidor: %s"), buffer);
                    fflush(stdout);
                    ReleaseMutex(hMutexConsole);
                }
                if (_tcsicmp(buffer, COMANDO_DESLIGAR_CLIENTE) == 0) run = false;
            }
            else run = false;
        }
        else Sleep(200);
    } // Fim while(run)
    return 0;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (run) _tprintf(_T("\nSinal de terminação recebido. A desligar...\n"));
        run = false;
        Sleep(200); // Pausa para a thread ver a flag
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL efetuarLoginServidor(globais* g) {
    if (g == NULL || g->hpipe == INVALID_HANDLE_VALUE || g->hpipe == NULL) {
        _tprintf(_T("[LOGIN_ERRO] Parâmetros inválidos para efetuarLoginServidor (g ou g->hpipe é NULL/Inválido).\n"));
        return FALSE;
    }
    TCHAR loginCmd[BUFFER_SIZE];
    TCHAR respostaLogin[BUFFER_SIZE];
    DWORD bytesLidosLogin;

    HRESULT hr = StringCchPrintf(loginCmd,
        _countof(loginCmd),
        _T("/login %s"),
        g->nomeJogador);

    if (FAILED(hr)) {
        _tprintf(_T("[LOGIN_ERRO] Falha ao formatar o comando de login (Erro StringCchPrintf: 0x%08X).\n"), hr);
        return FALSE;
    }

    LOG_DEBUG(_T("efetuarLoginServidor: Enviando comando de login: '%s' para o pipe %p"), loginCmd, g->hpipe);
    
    if (!enviarMensagemPipe(g->hpipe, loginCmd)) {
        _tprintf(_T("[CLIENTE_ERRO] Falha crítica ao enviar comando de login para o servidor.\n"));
        return FALSE;
    }

    ZeroMemory(respostaLogin, sizeof(respostaLogin));
    LOG_DEBUG(_T("efetuarLoginServidor: Aguardando resposta de login do servidor..."));

    if (ReadFile(g->hpipe,
        respostaLogin,
        (sizeof(respostaLogin) / sizeof(TCHAR) - 1) * sizeof(TCHAR), 
        &bytesLidosLogin,
        NULL)
        && bytesLidosLogin > 0)
    {
        respostaLogin[bytesLidosLogin / sizeof(TCHAR)] = _T('\0'); 
        LOG_DEBUG(_T("efetuarLoginServidor: Resposta do login recebida: '%s'"), respostaLogin);
        
        if (_tcsicmp(respostaLogin, _T("/login_ok")) == 0) {
			LOG_DEBUG(_T("efetuarLoginServidor: Login bem-sucedido para o jogador '%s'."), g->nomeJogador);
            return TRUE;
        }
        else {
            _tprintf(_T("[CLIENTE_ERRO] Login falhou. Motivo do servidor: %s\n"), respostaLogin);
            return FALSE;
        }
    }
    else 
        return FALSE;
}

void offJogador(globais* g) {
	if (g->dados != NULL) UnmapViewOfFile(g->dados);
	if (g->mapFile != NULL) CloseHandle(g->mapFile);
	if (g->hMutex != NULL) CloseHandle(g->hMutex);
	if (g->hEvento != NULL) CloseHandle(g->hEvento);
	if (g->hMutexConsole != NULL) CloseHandle(g->hMutexConsole);
	if (g->hStdin != NULL) SetConsoleMode(g->hStdin, g->originalStdInMode);
	g->originalCursorInfo.bVisible = TRUE;
	if (g->hStdout != NULL) SetConsoleCursorInfo(g->hStdout, &g->originalCursorInfo);
	if (g->hpipe != NULL) CloseHandle(g->hpipe);
	g->hpipe = NULL;
	g->dados = NULL;
	g->mapFile = NULL;
	g->hMutex = NULL;
	g->hEvento = NULL;
	g->hMutexConsole = NULL;
    SetConsoleCtrlHandler(CtrlHandler, FALSE); // Desregistar
}

int setup(globais* g) {
    int erros = 0;

	LOG_DEBUG(_T("SETUP: Iniciando setup...\n"));

    // Registar CtrlHandler
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) 
        LOG_DEBUG(_T("SETUP: Não foi possível definir o handler da consola (%lu).\n"), GetLastError());
    else
        LOG_DEBUG(_T("SETUP: CtrlHandler registado.\n"));

    // Obter Handles Standard da Consola
    g->hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    LOG_DEBUG(_T("SETUP: GetStdHandle(STD_OUTPUT_HANDLE) retornou: %p\n"), g->hStdout);
    g->hStdin = GetStdHandle(STD_INPUT_HANDLE);
    LOG_DEBUG(_T("SETUP: GetStdHandle(STD_INPUT_HANDLE) retornou: %p\n"), g->hStdin);

    if (g->hStdout == INVALID_HANDLE_VALUE || g->hStdin == INVALID_HANDLE_VALUE) {
        LOG_DEBUG(_T("SETUP ERRO FATAL: Não foi possível obter handles da consola (Stdout/Stdin). GetLastError: %lu\n"), GetLastError());
        return -1;
    }
    LOG_DEBUG(_T("SETUP: Handles Stdout/Stdin obtidos com sucesso.\n"));

    // Criar Mutex da Consola
    g->hMutexConsole = CreateMutex(NULL, FALSE, NOME_MUTEX_CONSOLE);
    if (g->hMutexConsole == NULL) {
        LOG_DEBUG(_T("SETUP ERRO FATAL: Falha ao criar Mutex da Consola (%lu).\n"), GetLastError());
        return -2;
    }
    LOG_DEBUG(_T("SETUP: Mutex da consola criado com sucesso.\n"));

    LOG_DEBUG(_T("SETUP: Tentando conectar ao pipe do servidor..."));
    if (!createPipe(g)) {
        LOG_DEBUG(_T("SETUP ERRO: Falha ao conectar ao pipe do servidor."));
        _tprintf(TEXT("[CLIENTE_ERRO] Não foi possível conectar ao servidor.\n"));
        return ++erros; 
    }
    LOG_DEBUG(_T("SETUP: Conectado ao pipe do servidor com sucesso (pipe: %p).\n"), g->hpipe);

	// Efetuar Login no Servidor
    LOG_DEBUG(_T("SETUP: Tentando efetuar login no servidor como '%s'..."), g->nomeJogador);
    if (!efetuarLoginServidor(g)) {
        LOG_DEBUG(_T("SETUP ERRO: Falha no processo de login com o servidor."));
        return ++erros;
    }
    LOG_DEBUG(_T("SETUP: Login no servidor como '%s' bem-sucedido.\n"), g->nomeJogador);

    // Configurar Cursor (Esconder)
    LOG_DEBUG(_T("SETUP: Tentando obter mutex da consola para config do cursor...\n"));
    if (WaitForSingleObject(g->hMutexConsole, 5000) == WAIT_OBJECT_0) {
        LOG_DEBUG(_T("SETUP: Mutex da consola obtido para config do cursor.\n"));
        if (!GetConsoleCursorInfo(g->hStdout, &g->originalCursorInfo)) {
            LOG_DEBUG(_T("SETUP ERRO: Não foi possível obter info original do cursor (%lu).\n"), GetLastError());
            erros++;
        }
        else {
            LOG_DEBUG(_T("SETUP: Info original do cursor obtida.\n"));
            CONSOLE_CURSOR_INFO tempCursorInfo = g->originalCursorInfo;
            tempCursorInfo.bVisible = FALSE;

            if (!SetConsoleCursorInfo(g->hStdout, &tempCursorInfo))
                LOG_DEBUG(_T("SETUP: Não foi possível esconder o cursor (%lu).\n"), GetLastError());
            else
                LOG_DEBUG(_T("SETUP: Cursor escondido.\n"));
        }
        ReleaseMutex(g->hMutexConsole);
        LOG_DEBUG(_T("SETUP: Mutex da consola libertado após config do cursor.\n"));
    }
    else {
        LOG_DEBUG(_T("SETUP ERRO: Falha ao obter mutex da consola para config do cursor. GetLastError(): %lu\n"), GetLastError());
        erros++;
    }

    // Configurar Modo da Consola de Input (Stdin)
    DWORD currentStdInModeVal = 0;

    LOG_DEBUG(_T("SETUP: Prestes a chamar GetConsoleMode para Stdin (Handle: %p).\n"), g->hStdin);
    if (g->hStdin == INVALID_HANDLE_VALUE || g->hStdin == NULL) {
        LOG_DEBUG(_T("SETUP ERRO: Handle Stdin inválido antes de GetConsoleMode.\n"));
        erros++;
    }
    else if (!GetConsoleMode(g->hStdin, &currentStdInModeVal)) {
        LOG_DEBUG(_T("SETUP ERRO: Não foi possível obter modo do stdin (%lu).\n"), GetLastError());
        erros++;
    }
    else {
        LOG_DEBUG(_T("SETUP: GetConsoleMode OK. Modo atual do stdin: %lu (0x%lX)\n"), currentStdInModeVal, currentStdInModeVal);
        g->originalStdInMode = currentStdInModeVal;
        DWORD newStdInMode = g->originalStdInMode;

        newStdInMode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
        
        LOG_DEBUG(_T("SETUP: Tentando definir novo modo do stdin: %lu (0x%lX)\n"), newStdInMode, newStdInMode);
        if (!SetConsoleMode(g->hStdin, newStdInMode))
            LOG_DEBUG(_T("SETUP AVISO: Não foi possível definir novo modo do stdin (%lu).\n"), GetLastError());
        else
            LOG_DEBUG(_T("SETUP: Novo modo do stdin definido com sucesso.\n"));
    }

    // Abrir Recursos da Memória Partilhada
    LOG_DEBUG(_T("SETUP: Abrindo recursos da Memória Partilhada...\n"));
    g->hMutex = OpenMutex(SYNCHRONIZE, FALSE, NOME_MUTEX);
    if (g->hMutex == NULL) { 
        LOG_DEBUG(_T("SETUP ERRO: Falha ao abrir Mutex da MP '%s' (%lu).\n"), NOME_MUTEX, GetLastError()); 
        erros++; 
    }
    else 
        LOG_DEBUG(_T("SETUP: Mutex da MP aberto.\n"));

    g->hEvento = OpenEvent(SYNCHRONIZE, FALSE, NOME_EVENTO);
    if (g->hEvento == NULL) { 
        LOG_DEBUG(_T("SETUP ERRO: Falha ao abrir Evento da MP '%s' (%lu).\n"), NOME_EVENTO, GetLastError()); 
        erros++; 
    }
    else 
        LOG_DEBUG(_T("SETUP: Evento da MP aberto.\n"));

    g->mapFile = OpenFileMapping(FILE_MAP_READ, FALSE, NOME_MP);
    if (g->mapFile == NULL) {
        LOG_DEBUG(_T("SETUP ERRO: Falha ao abrir Mapeamento de Ficheiro da MP '%s' (%lu).\n"), NOME_MP, GetLastError());
        erros++;
    }
    else {
        LOG_DEBUG(_T("SETUP: Mapeamento de Ficheiro da MP aberto.\n"));
        g->dados = (MP*)MapViewOfFile(g->mapFile, FILE_MAP_READ, 0, 0, sizeof(MP));
        if (g->dados == NULL) {
            LOG_DEBUG(_T("SETUP ERRO: Falha ao mapear View da MP '%s' (%lu).\n"), NOME_MP, GetLastError());
            erros++;
        }
        else
            LOG_DEBUG(_T("SETUP: View da MP mapeada.\n"));
    }

    if (erros > 0)
        LOG_DEBUG(_T("SETUP: Setup concluído com %d erro(s).\n"), erros);
    else
        LOG_DEBUG(_T("SETUP: Setup concluído com sucesso.\n"));

    return erros;
}
int _tmain(int argc, LPTSTR argv[]) {

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    globais g = { 0 };

    LOG_DEBUG(_T("CLIENTE: Chamando setup...\n"));
	_tprintf(TEXT("Digite o nome do jogador (até %d caracteres): "), NAME_SIZE - 1);
	fflush(stdout);
    if (_getts_s(g.nomeJogador, NAME_SIZE) == NULL || _tcslen(g.nomeJogador) == 0) {
        _tprintf(TEXT("Nome do jogador não pode ser vazio. Encerrando.\n"));
        return -1;
	}
    g.nomeJogador[_tcscspn(g.nomeJogador, _T("\r\n"))] = _T('\0');

    if (setup(&g) != 0) {
        _tprintf(TEXT("Falha no setup. Encerrando.\n"));
        offJogador(&g);
        _tprintf(TEXT("\nPressione Enter para sair.\n"));
        _gettchar();
        return -1;
    }

    HANDLE hEscutarInput = CreateThread(NULL, 0, threadEscutarInput, &g, 0, NULL);
	HANDLE hPipeRead = CreateThread(NULL, 0, receberComandosPipe, &g, 0, NULL);
    HANDLE hLetrasOutput = CreateThread(NULL, 0, threadLetrasOutput, &g, 0, NULL);

    HANDLE hThreads[] = { hEscutarInput, hLetrasOutput, hPipeRead };
    
	if (hEscutarInput != NULL && hLetrasOutput != NULL && hPipeRead != NULL) 
       WaitForMultipleObjects(3, hThreads, TRUE, INFINITE);

	if (hPipeRead) CloseHandle(hPipeRead);
    if (hEscutarInput) CloseHandle(hEscutarInput);
    if (hLetrasOutput) CloseHandle(hLetrasOutput);
	offJogador(&g);

    return 0;
}