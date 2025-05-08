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

typedef struct {
    MP* dados;
    HANDLE hMutex;
    HANDLE hEvento;

	HANDLE mapFile;
} memoria_partilhada;

#define NAME_SIZE 20
#define MAXJOGADORES 20

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
    sair,
    jogs,
    pont
} comando_jogador;

comando_jogador checkComandoJogador(const TCHAR* comando) {
    if (comando == NULL) return -1;

    if (strcmp(comando, "/sair") == 0) return sair;
    if (strcmp(comando, "/jogs") == 0) return jogs;
    if (strcmp(comando, "/iniciarbot") == 0) return pont;
    
    return -1;
}

typedef enum {
    listar,
    excluir,
    iniciarbot,
    acelerar,
    travar,
    encerrar
} comando_admin;

comando_admin checkComandoAdmin(const TCHAR* comando) {
    if (comando == NULL) return -1;

    if (strcmp(comando, "listar") == 0) return listar;
    if (strcmp(comando, "excluir") == 0) return excluir;
    if (strcmp(comando, "iniciarbot") == 0) return iniciarbot;
    if (strcmp(comando, "acelerar") == 0) return acelerar;
    if (strcmp(comando, "travar") == 0) return travar;
    if (strcmp(comando, "encerrar") == 0) return encerrar;

    return -1;
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

DWORD WINAPI threadGeradorLetras(memoria_partilhada* mp) {
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
    HANDLE hpipe = CreateNamedPipe(
        PIPE_NAME, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        MAXJOGADORES, BUFFER_SIZE, BUFFER_SIZE, 0, NULL);

    if (hpipe == INVALID_HANDLE_VALUE) {
        _tprintf(TEXT("Erro ao criar o pipe servidor duplex: %lu\n"), GetLastError());
    }
    else {
        _tprintf(TEXT("Pipe servidor '%s' criado. Aguardando clientes...\n"), PIPE_NAME);
        }
    return hpipe;
}

BOOL enviarMensagem(HANDLE hpipe, const TCHAR* mensagem) {
	DWORD bytesEscritos;
	BOOL resultado = WriteFile(hpipe, mensagem, (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR), &bytesEscritos, NULL);
	if (!resultado)
		_tprintf(TEXT("Erro ao enviar mensagem pelo pipe: %lu\n"), GetLastError());
	return resultado;
}

DWORD WINAPI receberComandos() {
	while (run) {
        HANDLE hpipe = CriarPipeServidorDuplex();
		if (hpipe == INVALID_HANDLE_VALUE) {
			_tprintf(TEXT("Erro ao criar o pipe: %lu\n"), GetLastError());
			if (!run) break;
            continue;
		}

		BOOL conexao = ConnectNamedPipe(hpipe, NULL);
		if (!conexao && GetLastError() != ERROR_PIPE_CONNECTED) {
			_tprintf(TEXT("Erro ao conectar ao pipe: %lu\n"), GetLastError());
			CloseHandle(hpipe);
            if (!run) break;
            continue;
		}


        TCHAR buffer[BUFFER_SIZE];
        DWORD bytesLidos;
		BOOL clientConectado = TRUE;

        while (clientConectado)
        {
            if (!run) {
				clientConectado = FALSE;
				continue;
            }

            BOOL leitura = ReadFile(hpipe, buffer, (sizeof(buffer) / sizeof(TCHAR) - 1) * sizeof(TCHAR), &bytesLidos, NULL);
            if (leitura && bytesLidos > 0) {
                buffer[bytesLidos / sizeof(TCHAR)] = '\0';
                _tprintf(TEXT("\nComando recebido: %s"), buffer);
            }
            else
                clientConectado = FALSE;
        }

		FlushFileBuffers(hpipe);
		DisconnectNamedPipe(hpipe);
		CloseHandle(hpipe);
	}

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
	
	byte erro = setup(&mp);
	if (erro != 0) {
		offArbitro(&mp);
        return erro;
	}

	SetConsoleCtrlHandler(CtrlHandler, TRUE);

	HANDLE hThreads[2];
	hThreads[0] = CreateThread(NULL, 0, threadGeradorLetras, &mp, 0, NULL);
	hThreads[1] = CreateThread(NULL, 0, receberComandos, NULL, 0, NULL);

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