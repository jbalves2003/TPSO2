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

//#define DEBUG_MODE 

#ifdef DEBUG_MODE
#define LOG_DEBUG(format, ...) _tprintf(_T("[DEBUG] :%d: " format _T("\n")), __LINE__, ##__VA_ARGS__)
#else
#define LOG_DEBUG(FORMAT, ...)
#endif

#define PIPE_NAME _T("\\\\.\\pipe\\pipe")
#define BUFFER_SIZE 512

#define RITMO 3 * 1000

#define NAME_SIZE 20
#define MAXJOGADORES 20
#define NOME_MUTEX_JOGADORES NULL // Mutex para proteger acesso à lista de jogadores

#define COMANDO_DESLIGAR_CLIENTE _T("/sair")


volatile bool run = TRUE;
volatile HANDLE g_hPipeServidor = NULL;

typedef struct {
    MP* dados;
    HANDLE hMutex;
    HANDLE hEvento;

	HANDLE mapFile;
} memoria_partilhada;

typedef struct {
    HANDLE hPipeCliente;
    int id_jogador;
    TCHAR nome[NAME_SIZE];
    int pontos;
} jogador;

typedef struct {
    jogador jogadores[MAXJOGADORES];
    int num_jogadores;
} lista_jogadores;
lista_jogadores g_listaJogadores = { 0 };
HANDLE g_hMutexJogadores = NULL;

typedef enum {
    invalido_jogador = -1,
    sair,
    jogs,
    pont
} comando_jogador;

// talvez adicionar defines para todos os comandos
comando_jogador checkComandoJogador(const TCHAR* comando) {
    if (comando == NULL) return invalido_jogador;
    
	if (_tcsicmp(comando, COMANDO_DESLIGAR_CLIENTE) == 0) return sair;
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

void enviarMensagemPipe(HANDLE hPipe, const TCHAR* mensagem) {
    if (hPipe == NULL || mensagem == NULL) return;
    
    DWORD bytesEscritos;
    BOOL resultado = WriteFile(hPipe, mensagem, (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR), &bytesEscritos, NULL);
    
    if (!resultado || bytesEscritos != (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR))
        LOG_DEBUG(_T("Erro ao enviar comando '%s' pelo pipe: %lu\n"), mensagem, GetLastError());
}

TCHAR* listarJogadores() {
    TCHAR* buffer = (TCHAR*)malloc(BUFFER_SIZE * sizeof(TCHAR));
    if (buffer == NULL) return NULL;
	ZeroMemory(buffer, BUFFER_SIZE * sizeof(TCHAR));

    if (WaitForSingleObject(g_hMutexJogadores, INFINITE) == WAIT_OBJECT_0) {

        if (g_listaJogadores.num_jogadores == 0) {
			StringCchCopy(buffer, BUFFER_SIZE, _T("Nenhum jogador conectado.\n"));
        }
        else {
            StringCchCopy(buffer, BUFFER_SIZE, _T("Jogadores conectados:\n"));
            for (int i = 0; i < g_listaJogadores.num_jogadores; i++) {
                StringCchCat(buffer, BUFFER_SIZE, g_listaJogadores.jogadores[i].nome);
                StringCchCat(buffer, BUFFER_SIZE, _T("\n"));
            }
        }
        ReleaseMutex(g_hMutexJogadores);

    } else 
        LOG_DEBUG(_T("listarJogadores: ERRO ao aceder lista.\n"));
    
	return buffer;
}

BOOL removerJogador(int id_jogador) {
    if (id_jogador < 0) return FALSE;
	LOG_DEBUG(_T("removerJogador: ID do jogador %d.\n"), id_jogador);

    if (WaitForSingleObject(g_hMutexJogadores, INFINITE) != WAIT_OBJECT_0) {
		LOG_DEBUG(_T("removerJogador: ERRO ao aceder lista de jogadores.\n"));
		return FALSE;
	}
    if (g_listaJogadores.num_jogadores <= 0) {
        ReleaseMutex(g_hMutexJogadores);
        return FALSE;
    }

    for (int i = 0; i < g_listaJogadores.num_jogadores; i++) {
        if (g_listaJogadores.jogadores[i].id_jogador == id_jogador) {
            _tprintf(_T("Desconectando jogador: %s\n"), g_listaJogadores.jogadores[i].nome);
            g_listaJogadores.jogadores[i] = g_listaJogadores.jogadores[--g_listaJogadores.num_jogadores];
			ZeroMemory(&g_listaJogadores.jogadores[g_listaJogadores.num_jogadores - 1], sizeof(jogador));
            break;
        }
    }

    ReleaseMutex(g_hMutexJogadores);
	LOG_DEBUG(_T("removerJogador: Jogador %d removido com sucesso.\n"), id_jogador);
	return TRUE;
}

int gerirComandoJogador(TCHAR* comando, jogador* cliente) {
    if (comando == NULL || cliente == NULL) return;
    comando_jogador cmd = checkComandoJogador(comando);
    switch (cmd) {
        case sair:
			if (!removerJogador(cliente->id_jogador))
                enviarMensagemPipe(cliente->hPipeCliente, _T("Tente digitar o comando outra vez"));
            else
				enviarMensagemPipe(cliente->hPipeCliente, _T("/sair"));
            return 1;
            break;
        case jogs:
			LOG_DEBUG(_T("Jogador %s solicitou lista de jogadores."), cliente->nome);
			TCHAR* lista = listarJogadores();
			enviarMensagemPipe(cliente->hPipeCliente, lista);
			free(lista);
			lista = NULL;
            break;
        case pont:
			LOG_DEBUG(_T("Jogador %s solicitou sua pontuação."), cliente->nome);
            TCHAR msgPontos[BUFFER_SIZE];
            _stprintf_s(msgPontos, _countof(msgPontos), _T("Sua pontuacao atual: %d"), cliente->pontos);
            enviarMensagemPipe(cliente->hPipeCliente, msgPontos);
            break;
        default:
			enviarMensagemPipe(cliente->hPipeCliente, _T("/erro ComandoInvalido"));
            break;
	}
	return 0;
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

	LOG_DEBUG(_T("Thread Gerador de Letras iniciada."));

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
			LOG_DEBUG(_T("threadGeradorLetras: ERRO ao aceder mutex.\n"));
            run = false;
            break;
        }
        Sleep(RITMO);
	} // fidel do while
    return 0;
}

HANDLE CriarPipeServidorDuplex() {
    HANDLE hPipe = CreateNamedPipe(
        PIPE_NAME, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        MAXJOGADORES, BUFFER_SIZE, BUFFER_SIZE, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE)
		LOG_DEBUG(_T("Erro ao criar pipe servidor: %lu\n"), GetLastError());
    else
        _tprintf(TEXT("\nPipe servidor '%s' criado. Aguardando clientes...\n"), PIPE_NAME);
        
    return hPipe;
}

// e preciso de trocar os timeouts para peek pipe, senao o readfile bloqueia
DWORD WINAPI receberComandos(LPVOID lpParam) {
	jogador* cliente = (jogador*)lpParam;
	if (cliente == NULL) return 1;

    LOG_DEBUG(_T("Thread de recebimento de comandos iniciada para o cliente %s (ID: %d, Pipe: %p)."),
		cliente->nome, cliente->id_jogador, cliente->hPipeCliente);

    HANDLE hPipeCliente = cliente->hPipeCliente;
    BOOL clienteConectado = TRUE;
    TCHAR buffer[BUFFER_SIZE];

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 3000;

	while (run && clienteConectado) {
		if (!run) { clienteConectado = FALSE; continue; }
           
            DWORD bytesLidos = 0;
            BOOL resultado = ReadFile(hPipeCliente, buffer, (sizeof(buffer) / sizeof(TCHAR) - 1) * sizeof(TCHAR), &bytesLidos, NULL);

            if (!run) { clienteConectado = FALSE; continue; }
            
            if (resultado && bytesLidos > 0) {
                buffer[bytesLidos / sizeof(TCHAR)] = _T('\0');
                LOG_DEBUG(_T("receberComandos: (Thread %lu, Pipe %p): Comando recebido do cliente '%s': %s"),GetCurrentThreadId(), hPipeCliente, cliente->nome, buffer);
				    
                if(gerirComandoJogador(buffer, cliente) == 1) {
                        _tprintf(TEXT("(Thread %lu, Pipe %p): Cliente '%s' desconectou.\n"),GetCurrentThreadId(), hPipeCliente, cliente->nome);
                        clienteConectado = FALSE;
                        continue;
				}
            } 
            else if (!resultado && 
                (GetLastError() != ERROR_BROKEN_PIPE || 
                 GetLastError() != ERROR_OPERATION_ABORTED || 
                 GetLastError() != ERROR_INVALID_HANDLE)) {
				LOG_DEBUG(_T("receberComandos: Erro ao ler do pipe: %lu\n"), GetLastError());
                clienteConectado = FALSE;
            }
	}

	enviarMensagemPipe(hPipeCliente, COMANDO_DESLIGAR_CLIENTE);

    if (hPipeCliente != NULL && hPipeCliente != INVALID_HANDLE_VALUE) {
		LOG_DEBUG(_T("receberComandos: (Thread %lu, Pipe %p): Desconectando cliente '%s'."),
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
	LOG_DEBUG(_T("Thread de gerenciamento de clientes iniciada."));
    
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
			LOG_DEBUG(_T("threadGereCliente ERRO: Falha ao conectar ao pipe (%lu)."), GetLastError());
            CloseHandle(hPipe);
            continue;
        }

		jogador* cliente = (jogador*)malloc(sizeof(jogador));
        if (cliente == NULL) {
			LOG_DEBUG(_T("threadGereCliente ERRO: Falha ao alocar memória para jogador."));
			DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }
        ZeroMemory(cliente, sizeof(jogador));
		cliente->hPipeCliente = hPipe;

		WaitForSingleObject(g_hMutexJogadores, INFINITE);

        if (g_listaJogadores.num_jogadores < MAXJOGADORES) {
            cliente->id_jogador = g_listaJogadores.num_jogadores;
			g_listaJogadores.jogadores[cliente->id_jogador].id_jogador = cliente->id_jogador;

            _stprintf_s(cliente->nome, NAME_SIZE, _T("Jogador%d"), cliente->id_jogador + 1);
            _tcscpy_s(g_listaJogadores.jogadores[cliente->id_jogador].nome, NAME_SIZE, cliente->nome);
			
            g_listaJogadores.jogadores[cliente->id_jogador].pontos = 0;
            g_listaJogadores.num_jogadores++;
            
            _tprintf(TEXT("\nARBITRO: Novo cliente conectado: %s (ID: %d, Pipe: %p)\n"), cliente->nome, cliente->id_jogador, hPipe);
        } else {
			enviarMensagemPipe(hPipe, COMANDO_DESLIGAR_CLIENTE);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            free(cliente);
            ReleaseMutex(g_hMutexJogadores);
            continue;
        }
		ReleaseMutex(g_hMutexJogadores);

		HANDLE  hThreadCliente = CreateThread(NULL, 0, receberComandos, cliente, 0, NULL);

        if (hThreadCliente == NULL) {
			LOG_DEBUG(_T("threadGereCliente ERRO: Falha ao criar thread para cliente (%lu)."), GetLastError());
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
        Sleep(200); 

        HANDLE hPipeParaDesbloquearConexao = InterlockedExchangePointer((PVOID*)&g_hPipeServidor, NULL);
        if (hPipeParaDesbloquearConexao != NULL && hPipeParaDesbloquearConexao != INVALID_HANDLE_VALUE) {
            _tprintf(TEXT("\nCTRL_HANDLER: Fechando pipe de conexão %p para desbloquear ConnectNamedPipe.\n"), hPipeParaDesbloquearConexao);
            CloseHandle(hPipeParaDesbloquearConexao);
        }
		
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
	if (g_hMutexJogadores != NULL) CloseHandle(g_hMutexJogadores);
    mp->dados = NULL;
    mp->mapFile = NULL;
    mp->hMutex = NULL;
	mp->hEvento = NULL;
	g_hMutexJogadores = NULL;
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

	g_hMutexJogadores = CreateMutex(NULL, FALSE, NOME_MUTEX_JOGADORES);
    if (g_hMutexJogadores == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Mutex para jogadores (%lu).\n"), GetLastError());
        erro++;
    }
    ZeroMemory(mp->dados, sizeof(MP));
	ZeroMemory(&g_listaJogadores, sizeof(lista_jogadores));

	if (erro > 0) {
		offArbitro(mp);
		return erro;
	}

    WaitForSingleObject(mp->hMutex, INFINITE);
    ZeroMemory(mp->dados, sizeof(MP));
	ZeroMemory(&g_listaJogadores, sizeof(lista_jogadores));
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
	
	LOG_DEBUG(_T("Iniciando o árbitro..."));

	int erro = setup(&mp);
	if (erro != 0) {
        _tprintf(TEXT("Falha no setup. Encerrando.\n"));
        offArbitro(&mp);
        _tprintf(TEXT("\nPressione Enter para sair.\n"));
        _gettchar();
        return erro;
	}

	LOG_DEBUG(_T("Árbitro iniciado com sucesso. Esperando por conexões de clientes..."));

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