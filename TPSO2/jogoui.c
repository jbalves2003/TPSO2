#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h> 
#include <time.h> 
#include <stdbool.h>

//#define PIPE_NAME _T("\\\\.\\pipe\\teste")

#define MAXLETRAS 10
#define RITMO 1 * 1000

TCHAR gerarLetra() { return (TCHAR)rand() % 26 + 65; }

void imprimirVetor(TCHAR* letras) {
    _puttchar(_T('\r'));

    for (int i = 0; i < MAXLETRAS; i++) {
        if (letras[i] != _T('\0'))
            _puttchar(letras[i]);
        else
            _puttchar(_T('_'));

        _puttchar(_T(' '));
        fflush(stdout);
    }
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

void apagaLetra(TCHAR* letras) {
    if (!verificaVetorVazio(letras))
        letras[MAXLETRAS - 1] = _T('\0');
}

int _tmain(int argc, LPTSTR argv[]) {

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    srand((unsigned int)time(NULL));
    TCHAR letras[MAXLETRAS] = { 0 };

    while (true)
    {
        apagaLetra(letras);
        escreveVetor(letras);
        imprimirVetor(letras);
        Sleep(RITMO);
    }

    return 0;
}