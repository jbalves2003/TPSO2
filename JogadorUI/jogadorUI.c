#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h> 
#include <time.h> 
#include <stdbool.h>
#include "mp.h"

#define PIPE_NAME _T("\\\\.\\pipe\\pipe")
#define BUFFER_SIZE 512

#define MAX_CONSOLE_EVENTS 128
#define INPUT_BUFFER_SIZE 64

volatile bool run = true;

typedef struct {
	// memoria partilhada
    MP* dados;
    HANDLE hMutex;
    HANDLE hEvento;

	HANDLE mapFile;

	// console
	HANDLE hStdin;        // Handle para o INPUT da consola
	DWORD originalStdInMode; // Para guardar o modo original do stdin
	HANDLE hStdout;
	CONSOLE_CURSOR_INFO originalCursorInfo;
} globais;

// Fncao para limpar linha da consola
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
    COORD coord = { 0, 0 }; // Coordenada de início
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

DWORD WINAPI threadEscutarInput(LPVOID lpParam) {
	globais* g = (globais*)lpParam;

    INPUT_RECORD inputRecord[MAX_CONSOLE_EVENTS]; // Buffer para eventos de input
    DWORD numEventsRead;
    TCHAR userInputBuffer[INPUT_BUFFER_SIZE]; // Buffer para guardar o texto digitado
    DWORD userInputLen = 0; // Comprimento atual do texto digitado

    const SHORT INPUT_LINE = 2; // Linha para input
    TCHAR prompt[] = _T("input>"); // Ajustado
    DWORD promptLen = (DWORD)_tcslen(prompt);
    DWORD lenWritten;

    // Escrever o prompt inicial
    COORD promptCoord = { 0, INPUT_LINE };
    limparLinha(INPUT_LINE, g->hStdout);
    WriteConsoleOutputCharacter(g->hStdout, prompt, promptLen, promptCoord, &lenWritten);
    SHORT inputStartX = (SHORT)promptLen;

    moverCursor(inputStartX, INPUT_LINE, g->hStdout); // Posicionar cursor inicial

    while (run) {

        if (!ReadConsoleInput(g->hStdin, inputRecord, MAX_CONSOLE_EVENTS, &numEventsRead)) {
            if (run) { _tprintf(_T("Erro ReadConsoleInput (%lu)\n"), GetLastError()); }
            run = false;
            continue;
        }

        // Processar cada evento lido
        for (DWORD i = 0; i < numEventsRead; ++i) {
            // Apenas processar eventos de teclado e quando a tecla é pressionada
            if (inputRecord[i].EventType == KEY_EVENT &&
                inputRecord[i].Event.KeyEvent.bKeyDown)
            {
                TCHAR ch = inputRecord[i].Event.KeyEvent.uChar.UnicodeChar;
                WORD vkCode = inputRecord[i].Event.KeyEvent.wVirtualKeyCode;

                // ---Lidar com Teclas ---
                if (vkCode == VK_RETURN) { // Enter Pressionado

                    userInputBuffer[userInputLen] = _T('\0');
                    if (userInputLen == 1 && (_tcsicmp(userInputBuffer, _T("q")) == 0)) {
                        _tprintf(_T("\n'q' detectado. Terminando...\n"));
                        run = false;
                    }

                    userInputLen = 0;
                    userInputBuffer[0] = _T('\0');
                    moverCursor(0, INPUT_LINE, g->hStdout);
                    limparLinha(INPUT_LINE, g->hStdout);

                    WriteConsoleOutputCharacter(g->hStdout, prompt, promptLen, promptCoord, &lenWritten);
                    moverCursor(inputStartX, INPUT_LINE, g->hStdout);

                }
                else if (vkCode == VK_BACK) { // Backspace Pressionado
                    if (userInputLen > 0) {
                        userInputLen--;
                        SHORT currentX = inputStartX + (SHORT)userInputLen;
                        COORD backspaceCoord = { currentX, INPUT_LINE };

                        WriteConsoleOutputCharacter(g->hStdout, _T(" "), 1, backspaceCoord, &lenWritten);

                        moverCursor(currentX, INPUT_LINE, g->hStdout);
                    }
                }
                else if (ch >= _T(' ') && userInputLen < INPUT_BUFFER_SIZE - 1) {
                    userInputBuffer[userInputLen] = ch;

                    SHORT currentX = inputStartX + (SHORT)userInputLen;
                    COORD charCoord = { currentX, INPUT_LINE };

                    WriteConsoleOutputCharacter(g->hStdout, &ch, 1, charCoord, &lenWritten);

                    userInputLen++;

                    moverCursor(currentX + 1, INPUT_LINE, g->hStdout);
                }
            }
        }

        Sleep(100);
    }

    limparLinha(INPUT_LINE, g->hStdout);
    SetConsoleCursorInfo(g->hStdout, &g->originalCursorInfo);
    return 0;
}

DWORD threadLetrasOutput(LPVOID lpParam) {
	globais* g = (globais*)lpParam;

	Sleep(200); 

    while (run) {
        if (WaitForSingleObject(g->hEvento, INFINITE) == WAIT_OBJECT_0) {
            if (WaitForSingleObject(g->hMutex, INFINITE) == WAIT_OBJECT_0) {
               // limparLinha(0, g->hConsole);
				TCHAR letrasCopia[MAXLETRAS];
                memcpy(letrasCopia, g->dados->letras, MAXLETRAS * sizeof(TCHAR));
                ReleaseMutex(g->hMutex);

                imprimirVetor(letrasCopia, g->hStdout);
            }
        }
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

void offJogador(globais* g) {
	if (g->dados != NULL) UnmapViewOfFile(g->dados);
	if (g->mapFile != NULL) CloseHandle(g->mapFile);
	if (g->hMutex != NULL) CloseHandle(g->hMutex);
	if (g->hEvento != NULL) CloseHandle(g->hEvento);
	if (g->hStdin != NULL) SetConsoleMode(g->hStdin, g->originalStdInMode);
	g->originalCursorInfo.bVisible = TRUE;
	if (g->hStdout != NULL) SetConsoleCursorInfo(g->hStdout, &g->originalCursorInfo);
	g->dados = NULL;
	g->mapFile = NULL;
	g->hMutex = NULL;
	g->hEvento = NULL;
    SetConsoleCtrlHandler(CtrlHandler, FALSE); // Desregistar
}

int setup(globais* g) {
    int erro = 0;
    
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) 
        _tprintf(_T("ERRO: Não foi possível definir o handler da consola.\n"));
    
    g->hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    g->hStdin = GetStdHandle(STD_INPUT_HANDLE);    // Para modos de consola
    if (g->hStdout == INVALID_HANDLE_VALUE || g->hStdin == INVALID_HANDLE_VALUE) {
        _tprintf(_T("ERRO: Não foi possível obter handles da consola.\n"));
		erro++;
		return -1;
    }

    if (!GetConsoleCursorInfo(g->hStdout, &g->originalCursorInfo)) {
        _tprintf(_T("ERRO: Não foi possível obter info do cursor.\n"));
		erro++;
    }

    g->originalCursorInfo.bVisible = FALSE;
    if (!SetConsoleCursorInfo(g->hStdout, &g->originalCursorInfo))
        _tprintf(_T("AVISO: Não foi possível esconder o cursor.\n"));

    DWORD currentStdInMode;
    if (!GetConsoleMode(g->hStdin, &currentStdInMode)) {
        _tprintf(_T("ERRO: Não foi possível obter modo da consola de input.\n"));
		erro++;
    }

    g->originalStdInMode = currentStdInMode;
    DWORD newStdInMode = g->originalStdInMode;

    newStdInMode &= ~ENABLE_QUICK_EDIT_MODE;
    newStdInMode &= ~ENABLE_ECHO_INPUT;      // <<< DESLIGAR ECHO
    newStdInMode &= ~ENABLE_LINE_INPUT;      // <<< DESLIGAR MODO LINHA
    newStdInMode |= ENABLE_EXTENDED_FLAGS;

    if (!SetConsoleMode(g->hStdin, newStdInMode))
        _tprintf(_T("AVISO: Não foi possível desativar QuickEdit Mode.\n"));

    g->hMutex = OpenMutex(SYNCHRONIZE, FALSE, NOME_MUTEX);
    if (g->hMutex == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Mutex.\n"));
		erro++;
    }

    g->hEvento = OpenEvent(SYNCHRONIZE, FALSE, NOME_EVENTO);
    if (g->hEvento == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Evento (%lu).\n"), GetLastError());
		erro++;
    }

    g->mapFile = OpenFileMapping(
        FILE_MAP_READ,
        FALSE,
        NOME_MP
    );
    if (g->mapFile == NULL) {
        _tprintf(_T("ERRO: Falha ao criar o arquivo de mapeamento (%lu).\n"), GetLastError());
		erro++;
    }

    g->dados = (MP*)MapViewOfFile(
        g->mapFile,
        FILE_MAP_READ,
        0,
        0,
        sizeof(MP));
    if (g->dados == NULL) {
        _tprintf(_T("ERRO: Falha ao mapear o arquivo (%lu).\n"), GetLastError());
		erro++;
    }

	return erro;
}

int _tmain(int argc, LPTSTR argv[]) {

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    globais g = { 0 };

	int erro = setup(&g);
	if (erro != 0) {
		offJogador(&g);
		return erro;
	}

    HANDLE hEscutarInput = CreateThread(NULL, 0, threadEscutarInput, &g, 0, NULL);
    HANDLE hLetrasOutput = CreateThread(NULL, 0, threadLetrasOutput, &g, 0, NULL);

    HANDLE hThreads[] = { hEscutarInput, hLetrasOutput };
    
	if (hEscutarInput != NULL && hLetrasOutput != NULL) 
       WaitForMultipleObjects(2, hThreads, TRUE, INFINITE);

    if (hEscutarInput) CloseHandle(hEscutarInput);
    if (hLetrasOutput) CloseHandle(hLetrasOutput);
	offJogador(&g);

    return 0;
}