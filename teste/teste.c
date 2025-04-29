// Cliente.c (Novo Ficheiro/Projeto)
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <io.h>
#include <fcntl.h>
#include <strsafe.h> // Para _tcslen, StringCchGets

#define PIPE_NAME _T("\\\\.\\pipe\\pipe") // <<< USAR O MESMO NOME DO ARBITRO
#define INPUT_BUFFER_SIZE 512

int _tmain(VOID)
{
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    TCHAR  chInputBuf[INPUT_BUFFER_SIZE];
    DWORD  dwWritten;
    BOOL   fSuccess = FALSE;


#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    _tprintf(_T("Cliente iniciado. Digite mensagens para enviar ao Arbitro.\n"));
    _tprintf(_T("Digite 'sair' para terminar.\n"));

    // Loop principal do cliente
    while (1)
    {
        // 1. Ler input do utilizador na consola do cliente
        _tprintf(_T("> ")); // Prompt
        // Usar StringCchGets para ler input de forma segura
        HRESULT hr = StringCchGets(chInputBuf, INPUT_BUFFER_SIZE);

        if (FAILED(hr)) {
            _tprintf(_T("Erro ao ler input do utilizador.\n"));
            continue; // Tentar novamente
        }

        // Remover a nova linha (\n) que StringCchGets pode deixar (se aplic�vel)
        // size_t len = _tcslen(chInputBuf);
        // if (len > 0 && chInputBuf[len - 1] == _T('\n')) {
        //      chInputBuf[len - 1] = _T('\0');
        // }
        // Nota: _fgetts pode ser uma alternativa melhor para input de linha

       // 2. Verificar se � comando para sair
        if (_tcsicmp(chInputBuf, _T("q")) == 0) {
            _tprintf(_T("Comando 'sair' detectado. A terminar cliente...\n"));
            break; // Sai do loop while
        }

        // 3. Tentar conectar ao pipe do Arbitro
        hPipe = CreateFile(
            PIPE_NAME,      // nome do pipe (o mesmo do Arbitro)
            GENERIC_WRITE,  // acesso de ESCRITA
            0,              // sem partilha
            NULL,           // seguran�a default
            OPEN_EXISTING,  // S� abre se o pipe J� EXISTIR (criado pelo Arbitro)
            0,              // atributos default
            NULL);          // sem template

        // Verificar se a conex�o foi bem-sucedida
        if (hPipe == INVALID_HANDLE_VALUE) {
            DWORD dwError = GetLastError();
            // Se o pipe n�o existe (Arbitro n�o correu?) ou est� ocupado
            if (dwError == ERROR_FILE_NOT_FOUND) {
                _tprintf(TEXT("[ERRO] Pipe '%s' n�o encontrado. O Arbitro est� a correr?\n"), PIPE_NAME);
                Sleep(2000); // Esperar um pouco antes de tentar de novo
                continue; // Volta ao in�cio do loop para pedir novo input
            }
            else if (dwError == ERROR_PIPE_BUSY) {
                _tprintf(TEXT("[AVISO] Pipe est� ocupado. A aguardar...\n"));
                // Esperar que o pipe fique dispon�vel
                if (!WaitNamedPipe(PIPE_NAME, 5000)) { // Espera at� 5 segundos
                    _tprintf(TEXT("[ERRO] Pipe n�o ficou dispon�vel ap�s espera.\n"));
                    continue; // Tentar de novo no pr�ximo input
                }
                _tprintf(TEXT("[INFO] Pipe dispon�vel. A tentar conectar novamente...\n"));
                // Tentar conectar novamente na pr�xima itera��o do loop
                continue;
            }
            else {
                _tprintf(TEXT("[ERRO] N�o foi poss�vel abrir pipe '%s', GLE=%d.\n"), PIPE_NAME, dwError);
                // Erro mais grave, talvez sair? Por agora, tentar de novo.
                Sleep(2000);
                continue;
            }
        }

        // 4. Conex�o bem-sucedida, enviar a mensagem
        _tprintf(TEXT("[INFO] Conectado ao Arbitro. A enviar: \"%s\"\n"), chInputBuf);
        fSuccess = WriteFile(
            hPipe,            // handle do pipe
            chInputBuf,       // dados a enviar (a mensagem digitada)
            (DWORD)(_tcslen(chInputBuf) + 1) * sizeof(TCHAR), // Enviar a string + terminador nulo
            &dwWritten,       // bytes escritos
            NULL);            // s�ncrono

        if (!fSuccess) {
            _tprintf(TEXT("[ERRO] WriteFile para o pipe falhou, GLE=%d.\n"), GetLastError());
            // O Arbitro pode ter fechado a conex�o entretanto?
        }
        else {
            _tprintf(TEXT("[INFO] Mensagem enviada.\n"));
        }

        // 5. Fechar o handle do pipe DEPOIS de enviar a mensagem
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE; // Boa pr�tica

    } // Fim do loop while(1)

    _tprintf(_T("Cliente a terminar.\n"));
    return 0;
}