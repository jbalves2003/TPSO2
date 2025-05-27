#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include "mp.h"

#define PIPE_NAME _T("\\\\.\\pipe\\pipe")
#define BUFFER_SIZE 512

#define RITMO 3 * 1000

volatile bool run = TRUE;
volatile HANDLE g_hPipeServidor = NULL;

typedef struct {
    MP* dados;
    HANDLE hMutex;
    HANDLE hEvento;

	HANDLE mapFile;
} memoria_partilhada;

#define NAME_SIZE 20
#define MAXJOGADORES 20

typedef struct {
	HANDLE hPipeCliente;
    TCHAR nome[NAME_SIZE];
    int pontos;
} dados_cliente;


typedef struct {
    int id_jogador;
    TCHAR nome[NAME_SIZE];
    int pontos;
} jogador;

typedef struct {
	jogador jogadores[MAXJOGADORES];
	int num_jogadores;
} lista_jogadores;


typedef enum {
    invalido_jogador = -1,
    sair,
    jogs,
    pont
} comando_jogador;

comando_jogador checkComandoJogador(const TCHAR* comando) {
    if (comando == NULL) return invalido_jogador;
    
	if (_tcsicmp(comando, _T("/sair")) == 0) return sair;
    if (_tcsicmp(comando, _T("/jogs")) == 0) return jogs;
    if (_tcsicmp(comando, _T("/pont")) == 0) return pont;
	return invalido_jogador;
}

typedef enum {
    invalido_admin = -1,
    listar,
    excluir,
    iniciarbot,
    acelerar,
    travar,
    encerrar
} comando_admin;

comando_admin checkComandoAdmin(const TCHAR* comando) {
    if (comando == NULL) return invalido_admin;

	if (_tcsicmp(comando, _T("/listar")) == 0) return listar;
    if (_tcsicmp(comando, _T("/excluir")) == 0) return excluir;
    if (_tcsicmp(comando, _T("/iniciarbot")) == 0) return iniciarbot;
    if (_tcsicmp(comando, _T("/acelerar")) == 0) return acelerar;
    if (_tcsicmp(comando, _T("/travar")) == 0) return travar;
	if (_tcsicmp(comando, _T("/encerrar")) == 0) return encerrar;

	return invalido_admin;
}


TCHAR gerarLetra() { return (TCHAR)rand() % 26 + 65; }

bool verificaVetorVazio(TCHAR* letras) {
    for (int i = 0; i < MAXLETRAS; i++)
        if (letras[i] == _T('\0'))
            return true;

    return false;
}

bool escreveVetor(TCHAR* letras) {
    for (int i = 0; i < MAXLETRAS; i++) {
        if (letras[i] == _T('\0')) {
            letras[i] = gerarLetra();
            return true;
        }
    }
    return false;
}

bool apagaLetra(TCHAR* letras, int pos) {

    if (pos < 0 || pos >= MAXLETRAS) return false;

    letras[pos] = _T('\0');
    return true;
}

DWORD WINAPI threadGeradorLetras(LPVOID lpParam) {
	memoria_partilhada* mp = (memoria_partilhada*)lpParam;

    while (run)
    {
        
        if (WaitForSingleObject(mp->hMutex, INFINITE) == WAIT_OBJECT_0) {

            if (verificaVetorVazio(mp->dados->letras))
                escreveVetor(mp->dados->letras);
            else if (!verificaVetorVazio(mp->dados->letras))
                apagaLetra(mp->dados->letras, MAXLETRAS - 1);

			_tprintf(_T("\nGerador: "));
			for (int i = 0; i < MAXLETRAS; i++) {
				if (mp->dados->letras[i] != _T('\0'))
					_tprintf(_T("%c "), mp->dados->letras[i]);
				else
					_tprintf(_T("_ "));
			}
			fflush(stdout);

            ReleaseMutex(mp->hMutex);

			SetEvent(mp->hEvento);
        }
        else {
            _tprintf(_T("\nERRO: Não foi possível obter o mutex.\n"));
            run = false;
            break;
        }

        Sleep(RITMO);
    }
    return 0;
}

HANDLE CriarPipeServidorDuplex() {
    HANDLE hPipe = CreateNamedPipe(
        PIPE_NAME, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        MAXJOGADORES, BUFFER_SIZE, BUFFER_SIZE, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        _tprintf(TEXT("\nErro ao criar o pipe servidor duplex: %lu\n"), GetLastError());
    }
    else {
        _tprintf(TEXT("\nPipe servidor '%s' criado. Aguardando clientes...\n"), PIPE_NAME);
        }
    return hPipe;
}

BOOL enviarMensagem(HANDLE hpipe, const TCHAR* mensagem) {
	DWORD bytesEscritos;
	BOOL resultado = WriteFile(hpipe, mensagem, (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR), &bytesEscritos, NULL);
	if (!resultado)
		_tprintf(TEXT("Erro ao enviar mensagem pelo pipe: %lu\n"), GetLastError());
	return resultado;
}

DWORD WINAPI receberComandos(LPVOID lpParam) {

	dados_cliente* cliente = (dados_cliente*)lpParam;
	if (cliente == NULL) return 1;

    HANDLE hPipeCliente = cliente->hPipeCliente;
    BOOL clienteConectado = TRUE;
    TCHAR buffer[BUFFER_SIZE];

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 3000;
    /*
    if (!SetCommTimeouts(hPipeCliente, &timeouts)) {
        _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): AVISO - Falha ao definir timeouts: %lu.\n"),
            GetCurrentThreadId(), hPipeCliente, GetLastError());
		clienteConectado = FALSE;
    } else {
        _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Timeouts de leitura definidos para %dms.\n"),
            GetCurrentThreadId(), hPipeCliente, timeouts.ReadTotalTimeoutConstant);
    }*/

	while (run && clienteConectado) {
		if (!run) { clienteConectado = FALSE; continue; }

           
            DWORD bytesLidos = 0;
            BOOL resultado = ReadFile(hPipeCliente, buffer, (sizeof(buffer) / sizeof(TCHAR) - 1) * sizeof(TCHAR), &bytesLidos, NULL);

            if (!run) { clienteConectado = FALSE; continue; }
            
            if (resultado && bytesLidos > 0) {
                buffer[bytesLidos / sizeof(TCHAR)] = _T('\0');
                _tprintf(TEXT("\nARBITRO (Thread %lu, Pipe %p): Comando recebido: '%s'\n"),
                    GetCurrentThreadId(), hPipeCliente, buffer);

                comando_jogador cmdJogador = checkComandoJogador(buffer);
                if (cmdJogador == sair) {
                    _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Jogador '%s' saiu.\n"),
                        GetCurrentThreadId(), hPipeCliente, cliente->nome);
                    clienteConectado = FALSE;
                } else if (cmdJogador == jogs) {
                    _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Listando jogadores...\n"),
						GetCurrentThreadId(), hPipeCliente);
                } else if (cmdJogador == pont) {
                    _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Iniciando bot...\n"),
                        GetCurrentThreadId(), hPipeCliente);
                } else if (cmdJogador == -1) {
                    _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Comando desconhecido: '%s'\n"),
                        GetCurrentThreadId(), hPipeCliente, buffer);
                } else {
                    _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Comando inválido: '%s'\n"),
						GetCurrentThreadId(), hPipeCliente, buffer);
                }
            } 
            else if (!resultado && 
                (GetLastError() != ERROR_BROKEN_PIPE || 
                 GetLastError() != ERROR_OPERATION_ABORTED || 
                 GetLastError() != ERROR_INVALID_HANDLE)) {
                _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Erro ao ler do pipe: %lu\n"),
                    GetCurrentThreadId(), hPipeCliente, GetLastError());
                clienteConectado = FALSE;
            }

	}

    if (hPipeCliente != NULL && hPipeCliente != INVALID_HANDLE_VALUE) {
        _tprintf(TEXT("ARBITRO (Thread %lu, Pipe %p): Desconectando cliente '%s'.\n"),
        GetCurrentThreadId(), hPipeCliente, cliente->nome);
		FlushFileBuffers(hPipeCliente);
        DisconnectNamedPipe(hPipeCliente);
        CloseHandle(hPipeCliente);
	}

	free(cliente);
	return 0;
}

DWORD WINAPI threadGereCliente(LPVOID lpParam) {
    memoria_partilhada* mp = (memoria_partilhada*)lpParam;
    
    while (run) {
		HANDLE hPipe = INVALID_HANDLE_VALUE;
		InterlockedExchangePointer((PVOID*) &g_hPipeServidor, NULL);

		if (!run) break;

		hPipe = CriarPipeServidorDuplex();
        if (hPipe == INVALID_HANDLE_VALUE) continue;

		InterlockedExchangePointer((PVOID*)&g_hPipeServidor, hPipe);

        BOOL conexao = ConnectNamedPipe(hPipe, NULL);
		if (!run) break;

		InterlockedExchangePointer((PVOID*)&g_hPipeServidor, NULL);

        if (!conexao && GetLastError() != ERROR_PIPE_CONNECTED) {
            _tprintf(TEXT("\nErro ao conectar ao pipe: %lu\n"), GetLastError());
            CloseHandle(hPipe);
            continue;
        }

		dados_cliente* cliente = (dados_cliente*)malloc(sizeof(dados_cliente));
        if (cliente == NULL) {
            _tprintf(TEXT("\nERRO: Falha ao alocar memória para dados do cliente.\n"));
			DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }
        ZeroMemory(cliente, sizeof(dados_cliente));
		cliente->hPipeCliente = hPipe;

		HANDLE  hThreadCliente = CreateThread(NULL, 0, receberComandos, cliente, 0, NULL);

        if (hThreadCliente == NULL) {
            _tprintf(TEXT("\nERRO: Falha ao criar thread para cliente (%lu).\n"), GetLastError());
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            free(cliente);
        } else
        	CloseHandle(hThreadCliente);
    }

	InterlockedExchangePointer((PVOID*)&g_hPipeServidor, NULL);
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
		HANDLE hPipe = InterlockedExchangePointer((LPVOID*) &g_hPipeServidor, NULL);
        if (hPipe != NULL && hPipe != INVALID_HANDLE_VALUE)
            CloseHandle(hPipe);
		
        return TRUE;
    default:
        return FALSE;
    }
}

void offArbitro(memoria_partilhada *mp) {
	if (mp->dados != NULL) UnmapViewOfFile(mp->dados);
	if (mp->mapFile != NULL) CloseHandle(mp->mapFile);
	if (mp->hMutex != NULL) CloseHandle(mp->hMutex);
    if (mp->hEvento != NULL) CloseHandle(mp->hEvento);
    mp->dados = NULL;
    mp->mapFile = NULL;
    mp->hMutex = NULL;
	mp->hEvento = NULL;
	SetConsoleCtrlHandler(CtrlHandler, FALSE); // Desregistar
}

int setup(memoria_partilhada *mp) {
	int erro = 0;

    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
        _tprintf(_T("ERRO: Não foi possível definir o handler da consola no árbitro.\n"));

    mp->hMutex = CreateMutex(NULL, FALSE, NOME_MUTEX);
    if (mp->hMutex == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Mutex (%lu).\n"), GetLastError());
		erro++;
    }

    mp->hEvento = CreateEvent(NULL, FALSE, FALSE, NOME_EVENTO);
    if (mp->hEvento == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Evento (%lu).\n"), GetLastError());
		erro++;
    }

    mp->mapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(MP),
        NOME_MP
    );

    if (mp->mapFile == NULL) {
        _tprintf(_T("ERRO: Falha ao criar FileMapping (%lu).\n"), GetLastError());
        erro++;
    }

    mp->dados = (MP*)MapViewOfFile(
        mp->mapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(MP)
    );
    if (mp->dados == NULL) {
        _tprintf(_T("ERRO: Falha ao mapear a memória (%lu).\n"), GetLastError());
        erro++;
    }

	if (erro > 0) {
		offArbitro(mp);
		return erro;
	}

    WaitForSingleObject(mp->hMutex, INFINITE);
    ZeroMemory(mp->dados, sizeof(MP));
    ReleaseMutex(mp->hMutex);

	return erro;
}

int _tmain(int argc, LPTSTR argv[]) {

#ifdef UNICODE
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    srand((unsigned int)time(NULL));
    memoria_partilhada mp = { 0 };
	
	int erro = setup(&mp);
	if (erro != 0) {
		offArbitro(&mp);
        return erro;
	}

	SetConsoleCtrlHandler(CtrlHandler, TRUE);

	HANDLE hThreads[2];
	hThreads[0] = CreateThread(NULL, 0, threadGeradorLetras, &mp, 0, NULL);
	hThreads[1] = CreateThread(NULL, 0, threadGereCliente, &mp, 0, NULL);

	if (hThreads[0] == NULL || hThreads[1] == NULL) {
		_tprintf(_T("ERRO: Falha ao criar threads (%lu).\n"), GetLastError());
		if (hThreads[0] != NULL) CloseHandle(hThreads[0]);
		if (hThreads[1] != NULL) CloseHandle(hThreads[1]);
		offArbitro(&mp);
		return 1;
	}

	WaitForMultipleObjects(2, hThreads, TRUE, INFINITE);
	CloseHandle(hThreads[0]);
	CloseHandle(hThreads[1]);

	offArbitro(&mp);
    return 0;
}