#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h> 
#include <time.h> 
#include <stdbool.h>

//#define PIPE_NAME _T("\\\\.\\pipe\\teste")

#define MAXLETRAS 4
#define RITMO 3 * 1000

volatile bool run = true; 

HANDLE hConsole;                     
CONSOLE_CURSOR_INFO originalCursorInfo;
HANDLE hStdin;        // Handle para o INPUT da consola
DWORD originalStdInMode; // Para guardar o modo origina

HANDLE hMutex = NULL, hEvent = NULL;

TCHAR gerarLetra() { return (TCHAR)rand() % 26 + 65; }

void imprimirVetor(TCHAR* letras) {
    COORD coord = { 0, 0 }; // Coordenada de início
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    
    if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
        coord.Y = csbi.dwCursorPosition.Y; // Mantém a linha Y atual
    }

    SetConsoleCursorPosition(hConsole, coord);

    int charsWritten = 0;
    for (int i = 0; i < MAXLETRAS; i++) {
        TCHAR charToPrint = (letras[i] != _T('\0')) ? letras[i] : _T('_');
        _puttchar(charToPrint);
        _puttchar(_T(' '));
        charsWritten += 2; // Conta letra + espaço
    }

    // Limpa o resto da linha caso MAXLETRAS mude (ou para garantir)
    DWORD dwCharsWritten;
    FillConsoleOutputCharacter(hConsole, _T(' '), csbi.dwSize.X - charsWritten, // Nº de espaços a preencher
        (COORD) {
        (SHORT)charsWritten, coord.Y
    }, // Posição onde começar a preencher
        & dwCharsWritten);

    fflush(stdout);
}

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

DWORD WINAPI threadEscreve(LPVOID lpParam) {
	TCHAR* letras = (TCHAR*)lpParam;

	while (run) {
        bool event = false;

		if (WaitForSingleObject(hMutex, INFINITE) == WAIT_OBJECT_0) {
			event = escreveVetor(letras);
			ReleaseMutex(hMutex);
		}

		if (event)
			SetEvent(hEvent); 

		Sleep(RITMO);
	}
	return 0;
}

DWORD WINAPI threadApaga(LPVOID lpParam) {
	TCHAR* letras = (TCHAR*)lpParam;
	while (run) {
		bool event = false;

		if (WaitForSingleObject(hMutex, INFINITE) == WAIT_OBJECT_0) {
            
            if (!verificaVetorVazio(letras)) 
				event = apagaLetra(letras, MAXLETRAS - 1);

            ReleaseMutex(hMutex);
		}
            
		if (event)
			SetEvent(hEvent); 

		Sleep(RITMO);
    }
	return 0;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
        _tprintf(_T("\nSinal de terminação recebido. A desligar...\n"));
        run = false;
        if (hConsole != INVALID_HANDLE_VALUE) {
            SetConsoleCursorInfo(hConsole, &originalCursorInfo);
        }
        if (hEvent != NULL) SetEvent(hEvent);
        Sleep(200);
        return TRUE;
    default:
        return FALSE;
    }
}

int _tmain(int argc, LPTSTR argv[]) {

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
        _tprintf(_T("ERRO: Não foi possível definir o handler da consola.\n"));
        return 1;
    }

    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    hStdin = GetStdHandle(STD_INPUT_HANDLE);    // Para modos de consola
    if (hConsole == INVALID_HANDLE_VALUE || hStdin == INVALID_HANDLE_VALUE) {
        _tprintf(_T("ERRO: Não foi possível obter handles da consola.\n"));
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
        return 1;
    }

    if (!GetConsoleCursorInfo(hConsole, &originalCursorInfo)) {
        _tprintf(_T("ERRO: Não foi possível obter info do cursor.\n"));
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
        return 1;
    }

    CONSOLE_CURSOR_INFO cursorInfo = originalCursorInfo;
    cursorInfo.bVisible = FALSE; // Esconde o cursor
    if (!SetConsoleCursorInfo(hConsole, &cursorInfo))
        _tprintf(_T("AVISO: Não foi possível esconder o cursor.\n"));

    DWORD currentStdInMode;
    if (!GetConsoleMode(hStdin, &currentStdInMode)) {
        _tprintf(_T("ERRO: Não foi possível obter modo da consola de input.\n"));
        SetConsoleCursorInfo(hConsole, &originalCursorInfo); // Limpar cursor
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
        return 1;
    }
    originalStdInMode = currentStdInMode; 
    DWORD newStdInMode = originalStdInMode;
 
    newStdInMode &= ~ENABLE_QUICK_EDIT_MODE;

    newStdInMode |= ENABLE_EXTENDED_FLAGS;

    if (!SetConsoleMode(hStdin, newStdInMode))
        _tprintf(_T("AVISO: Não foi possível desativar QuickEdit Mode.\n"));

    hMutex = CreateMutex(NULL, FALSE, NULL);
    if (hMutex == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Mutex.\n"));
        SetConsoleCursorInfo(hConsole, &originalCursorInfo);
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
        return 3;
    }

    hEvent = CreateEvent(NULL,  // Atributos de segurança padrão
        FALSE, // Auto-reset event (volta a não sinalizado após um wait ser satisfeito)
        FALSE, // Estado inicial: não sinalizado
        NULL); // Sem nome
    if (hEvent == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Event.\n"));
        CloseHandle(hMutex);
        SetConsoleCursorInfo(hConsole, &originalCursorInfo);
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
        return 4;
    }

    srand((unsigned int)time(NULL));
    TCHAR letras[MAXLETRAS] = { 0 };
	
    HANDLE hescreve = CreateThread(NULL, 0, threadEscreve, letras, 0, NULL);
    HANDLE hapaga = CreateThread(NULL, 0, threadApaga, letras, 0, NULL);

    if (hescreve == NULL || hapaga == NULL) {
        _tprintf(_T("ERRO: Falha ao criar uma ou ambas as threads.\n"));
        if (hescreve) CloseHandle(hescreve);
        if (hapaga) CloseHandle(hapaga);
        SetConsoleCursorInfo(hConsole, &originalCursorInfo);
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
        return 2;
    }

    while (run) {
        if (!run) break;
		if (WaitForSingleObject(hEvent, 50) == WAIT_OBJECT_0) {
			if (WaitForSingleObject(hMutex, INFINITE) == WAIT_OBJECT_0) {
				imprimirVetor(letras);
				ReleaseMutex(hMutex);
			}
		}
    }

	WaitForSingleObject(hescreve, INFINITE);
	WaitForSingleObject(hapaga, INFINITE);

    SetConsoleCursorInfo(hConsole, &originalCursorInfo);
	SetConsoleMode(hStdin, originalStdInMode);

    CloseHandle(hescreve);
	CloseHandle(hapaga);
	CloseHandle(hMutex);
	CloseHandle(hEvent);

	SetConsoleCtrlHandler(CtrlHandler, FALSE); // Desregistar
    
    return 0;
}