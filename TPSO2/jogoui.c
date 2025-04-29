#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h> 
#include <time.h> 
#include <stdbool.h>

#define PIPE_NAME _T("\\\\.\\pipe\\pipe")
#define BUFFER_SIZE 512

#define MAXLETRAS 5
#define RITMO 3 * 1000

#define MAX_CONSOLE_EVENTS 128
#define INPUT_BUFFER_SIZE 64

volatile bool run = true;

HANDLE hConsole;
CONSOLE_CURSOR_INFO originalCursorInfo;
HANDLE hStdin;        // Handle para o INPUT da consola
DWORD originalStdInMode; // Para guardar o modo origina

HANDLE hMutex = NULL;

TCHAR gerarLetra() { return (TCHAR)rand() % 26 + 65; }

// Fncao para limpar linha da consola
void limparLinha(SHORT y) {
    COORD coord = { 0, y };

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD charsWritten;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;

    SetConsoleCursorPosition(hConsole, coord);
    FillConsoleOutputCharacter(hConsole, _T(' '), csbi.dwSize.X, coord, &charsWritten);
    SetConsoleCursorPosition(hConsole, coord);
}

void moverCursor(SHORT x, SHORT y) {
    COORD coord = { x, y };
    SetConsoleCursorPosition(hConsole, coord);
}

void imprimirVetor(TCHAR* letras) {
    COORD coord = { 0, 0 }; // Coordenada de início
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;
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

DWORD WINAPI threadGeradorLetras(LPVOID lpParam) {
    TCHAR* letras = (TCHAR*)lpParam;
	Sleep(1000);
    while (run)
    {
        bool flag = false;

        if (WaitForSingleObject(hMutex, INFINITE) == WAIT_OBJECT_0) {

            if (verificaVetorVazio(letras))
                flag = escreveVetor(letras);
            else if (!verificaVetorVazio(letras))
                flag = apagaLetra(letras, MAXLETRAS - 1);

            imprimirVetor(letras);

            ReleaseMutex(hMutex);
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

DWORD WINAPI threadEscutarInput(LPVOID lpParam) {
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
    limparLinha(INPUT_LINE);
    WriteConsoleOutputCharacter(hConsole, prompt, promptLen, promptCoord, &lenWritten);
    SHORT inputStartX = (SHORT)promptLen; 

    moverCursor(inputStartX, INPUT_LINE); // Posicionar cursor inicial

    while (run) {

        if (!ReadConsoleInput(hStdin, inputRecord, MAX_CONSOLE_EVENTS, &numEventsRead)) {
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
                    moverCursor(0, INPUT_LINE);
                    limparLinha(INPUT_LINE);   

                    WriteConsoleOutputCharacter(hConsole, prompt, promptLen, promptCoord, &lenWritten);
                    moverCursor(inputStartX, INPUT_LINE);

                }
                else if (vkCode == VK_BACK) { // Backspace Pressionado
                    if (userInputLen > 0) {
                        userInputLen--;
                        SHORT currentX = inputStartX + (SHORT)userInputLen;
                        COORD backspaceCoord = { currentX, INPUT_LINE };

                        WriteConsoleOutputCharacter(hConsole, _T(" "), 1, backspaceCoord, &lenWritten);

                        moverCursor(currentX, INPUT_LINE);
                    }
                }
                else if (ch >= _T(' ') && userInputLen < INPUT_BUFFER_SIZE - 1) {
                    userInputBuffer[userInputLen] = ch;
                 
                    SHORT currentX = inputStartX + (SHORT)userInputLen;
                    COORD charCoord = { currentX, INPUT_LINE };
                    
                    WriteConsoleOutputCharacter(hConsole, &ch, 1, charCoord, &lenWritten);
                    
                    userInputLen++;
                    
                    moverCursor(currentX + 1, INPUT_LINE);
                }
            }
        }

        Sleep(100);
    }

    limparLinha(INPUT_LINE);
    SetConsoleCursorInfo(hConsole, &originalCursorInfo);
    return 0;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
        _tprintf(_T("\nSinal de terminação recebido. A desligar...\n"));
        run = false;
        if (hStdin != INVALID_HANDLE_VALUE)
            SetConsoleMode(hStdin, originalStdInMode);
        if (hConsole != INVALID_HANDLE_VALUE)
            SetConsoleCursorInfo(hConsole, &originalCursorInfo);
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
    cursorInfo.bVisible = FALSE; 
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
    newStdInMode &= ~ENABLE_ECHO_INPUT;      // <<< DESLIGAR ECHO
    newStdInMode &= ~ENABLE_LINE_INPUT;      // <<< DESLIGAR MODO LINHA
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

    srand((unsigned int)time(NULL));
    TCHAR letras[MAXLETRAS] = { 0 };

    HANDLE hGeradorLetras = CreateThread(NULL, 0, threadGeradorLetras, letras, 0, NULL);
    HANDLE hEscutarInput = CreateThread(NULL, 0, threadEscutarInput, NULL, 0, NULL);

    if (hGeradorLetras == NULL || hEscutarInput == NULL) {
        _tprintf(_T("ERRO: Falha ao criar uma ou ambas as threads.\n"));
        if (hGeradorLetras) CloseHandle(hGeradorLetras);
        if (hEscutarInput) CloseHandle(hEscutarInput);
        CloseHandle(hMutex);
        SetConsoleMode(hStdin, originalStdInMode);
        SetConsoleCursorInfo(hConsole, &originalCursorInfo);
        SetConsoleCtrlHandler(CtrlHandler, FALSE);
        return 2;
    }

    HANDLE hThreads[] = { hGeradorLetras, hEscutarInput };
    WaitForMultipleObjects(2, hThreads, TRUE, INFINITE);

    SetConsoleCursorInfo(hConsole, &originalCursorInfo);
    SetConsoleMode(hStdin, originalStdInMode);

    for (int i = 0; i < sizeof(hThreads) / sizeof(hThreads[0]); ++i)
        if (hThreads[i] != NULL) 
            CloseHandle(hThreads[i]);

    CloseHandle(hMutex);

    SetConsoleCtrlHandler(CtrlHandler, FALSE); // Desregistar

    return 0;
}