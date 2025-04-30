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

#define PIPE_NAME _T("\\\\.\\pipe\\arbitro")
#define BUFFER_SIZE 512

#define RITMO 3 * 1000

volatile bool run = TRUE;

typedef struct {
    MP* dados;
    HANDLE hMutex;
    HANDLE hEvento;

	HANDLE mapFile;
} memoria_partilhada;


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

    HANDLE hGeradorLetras = CreateThread(NULL, 0, threadGeradorLetras, &mp, 0, NULL);
    if (hGeradorLetras != NULL) {
        WaitForSingleObject(hGeradorLetras, INFINITE);
		CloseHandle(hGeradorLetras);
    }

	offArbitro(&mp);
    return 0;
}