#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>
#include "mp.h"


//#define DEBUG_MODE
#ifdef DEBUG_MODE
#define LOG_DEBUG(format, ...) _tprintf(_T("[BOT_DEBUG] :%s:%d: " format _T("\n")), _T(__FILE__), __LINE__, ##__VA_ARGS__)
#else
#define LOG_DEBUG(FORMAT, ...)
#endif

#define PIPE_NAME _T("\\\\.\\pipe\\pipe")
#define BUFFER_SIZE 512
#define BOT_NAME_PREFIX _T("BotPlayer")

#define COMANDO_DESLIGAR_CLIENTE _T("/sair")

// --- Definições e Funções do Dicionário ---
#define NOME_FICHEIRO_DICIONARIO _T("dicionario.txt")
#define MAX_PALAVRAS_DICIONARIO 50000
#define TAMANHO_MAX_PALAVRA_DICIONARIO 30

TCHAR g_Dicionario[MAX_PALAVRAS_DICIONARIO][TAMANHO_MAX_PALAVRA_DICIONARIO];
int g_NumeroDePalavrasNoDicionario = 0;

// Parâmetros de comportamento do Bot
#define MIN_REACTION_MS 3000
#define MAX_REACTION_MS 7000
#define CHANCE_TO_SUBMIT_WORD 90 


#define CHANCE_BOT_TENTAR_PALAVRA_ERRADA 70 
#define CHANCE_BOT_DIGITAR_ERRADO 10    
#define MIN_LEN_PALAVRA_ERRADA 3
#define MAX_LEN_PALAVRA_ERRADA 7


volatile bool g_run_bot = TRUE;

typedef struct {
    MP* dados_mp;
    HANDLE hMutex_mp;
    HANDLE hEvento_mp;
    HANDLE hMapFile_mp;
    HANDLE hPipe;
    TCHAR bot_name[50];
    int reaction_time_ms;
} bot_globals;

BOOL carregarDicionario() {
    FILE* fp = NULL;
    errno_t err = _tfopen_s(&fp, NOME_FICHEIRO_DICIONARIO, _T("r, ccs=UTF-8"));

    if (err != 0 || fp == NULL) {
        _tprintf(TEXT("[BOT_ERRO]: Nao foi possivel abrir o ficheiro do dicionario '%s'. errno: %d\n"), NOME_FICHEIRO_DICIONARIO, err);
        LOG_DEBUG(_T("Erro ao abrir dicionario. Verifique se o ficheiro '%s' existe e tem permissao de leitura."), NOME_FICHEIRO_DICIONARIO);
        return FALSE;
    }

    _tprintf(TEXT("[BOT_INFO]: Carregando dicionario de '%s'...\n"), NOME_FICHEIRO_DICIONARIO);
    g_NumeroDePalavrasNoDicionario = 0;
    TCHAR linhaBuffer[TAMANHO_MAX_PALAVRA_DICIONARIO + 2];

    while (g_NumeroDePalavrasNoDicionario < MAX_PALAVRAS_DICIONARIO &&
        _fgetts(linhaBuffer, _countof(linhaBuffer), fp) != NULL)
    {
        linhaBuffer[_tcscspn(linhaBuffer, _T("\r\n"))] = _T('\0'); // Remove newline
        size_t lenPalavra = _tcslen(linhaBuffer);

        if (lenPalavra > 0 && lenPalavra < TAMANHO_MAX_PALAVRA_DICIONARIO && lenPalavra <= MAXLETRAS) {
            _tcscpy_s(g_Dicionario[g_NumeroDePalavrasNoDicionario], TAMANHO_MAX_PALAVRA_DICIONARIO, linhaBuffer);
            g_NumeroDePalavrasNoDicionario++;
        }
        else if (lenPalavra > MAXLETRAS) {
            LOG_DEBUG(_T("Palavra '%s' ignorada do dicionario (muito longa para o jogo - max %d)."), linhaBuffer, MAXLETRAS);
        }
    }

    fclose(fp);
    _tprintf(TEXT("[BOT_INFO]: Dicionario carregado com %d palavras.\n"), g_NumeroDePalavrasNoDicionario);

    if (g_NumeroDePalavrasNoDicionario == 0) {
        _tprintf(TEXT("[BOT_AVISO]: Nenhuma palavra carregada do dicionario. O ficheiro pode estar vazio ou mal formatado.\n"));
    }
    return TRUE;
}

void cleanup_bot(bot_globals* bg) {
    if (bg->dados_mp != NULL) UnmapViewOfFile(bg->dados_mp);
    bg->dados_mp = NULL;
    if (bg->hMapFile_mp != NULL) CloseHandle(bg->hMapFile_mp);
    bg->hMapFile_mp = NULL;
    if (bg->hMutex_mp != NULL) CloseHandle(bg->hMutex_mp);
    bg->hMutex_mp = NULL;
    if (bg->hEvento_mp != NULL) CloseHandle(bg->hEvento_mp);
    bg->hEvento_mp = NULL;
    if (bg->hPipe != NULL && bg->hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(bg->hPipe);
    }
    bg->hPipe = NULL;
    LOG_DEBUG(_T("Recursos do bot limpos."));
}

BOOL WINAPI CtrlHandler_Bot(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
        if (g_run_bot) {
            _tprintf(_T("\n[BOT:%s] Sinal de terminação (Ctrl-C/Close) recebido. A desligar...\n"), _T("NomeBot")); // Idealmente usar bg->bot_name se acessível
        }
        g_run_bot = FALSE;
        Sleep(200);
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL enviarMensagemPipeBot(HANDLE hPipe, const TCHAR* mensagem) {
    if (hPipe == NULL || hPipe == INVALID_HANDLE_VALUE || mensagem == NULL) {
        LOG_DEBUG(_T("enviarMensagemPipeBot: Parâmetros invalidos (pipe ou mensagem nulos)."));
        return FALSE;
    }
    DWORD bytesEscritos;
    DWORD lenBytes = (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR);

    BOOL resultado = WriteFile(hPipe, mensagem, lenBytes, &bytesEscritos, NULL);

    if (!resultado) {
        LOG_DEBUG(_T("Erro ao enviar mensagem '%s' pelo pipe: %lu"), mensagem, GetLastError());
    }
    else if (bytesEscritos != lenBytes) {
        LOG_DEBUG(_T("Aviso: Nem todos os bytes foram escritos para '%s'. Esperado: %lu, Escrito: %lu"), mensagem, lenBytes, bytesEscritos);
        return FALSE;
    }
    LOG_DEBUG(_T("Mensagem '%s' enviada com sucesso."), mensagem);
    return resultado;
}

DWORD WINAPI threadReceberComandosServidor(LPVOID lpParam) {
    bot_globals* bg = (bot_globals*)lpParam;
    TCHAR buffer[BUFFER_SIZE];
    LOG_DEBUG(_T("Thread de escuta do servidor para o bot '%s' iniciada."), bg->bot_name);

    while (g_run_bot) {
        DWORD bytesLidos = 0;
        COMMTIMEOUTS timeouts = { 0 };
        timeouts.ReadIntervalTimeout = 0;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 200;

        if (bg->hPipe == INVALID_HANDLE_VALUE || !SetCommTimeouts(bg->hPipe, &timeouts)) {
            if (g_run_bot) LOG_DEBUG(_T("Falha ao definir timeouts ou pipe invalido. %lu"), GetLastError());
            if (g_run_bot) Sleep(100);
            continue;
        }

        BOOL resultado = ReadFile(bg->hPipe, buffer, (BUFFER_SIZE - 1) * sizeof(TCHAR), &bytesLidos, NULL);

        if (!g_run_bot) break;

        if (resultado && bytesLidos > 0) {
            buffer[bytesLidos / sizeof(TCHAR)] = _T('\0');
            _tprintf(_T("\n[BOT:%s <- Servidor]: %s\n"), bg->bot_name, buffer);

            if (_tcsicmp(buffer, COMANDO_DESLIGAR_CLIENTE) == 0) {
                _tprintf(_T("[BOT:%s] Recebido comando para desligar do servidor.\n"), bg->bot_name);
                g_run_bot = FALSE;
                break;
            }

            if (_tcsstr(buffer, _T("/rejeitado")) != NULL || _tcsstr(buffer, _T("/kick")) != NULL) {
                _tprintf(_T("[BOT:%s] Bot rejeitado ou expulso pelo servidor. Desligando.\n"), bg->bot_name);
                g_run_bot = FALSE;
                break;
            }

        }
        else if (!resultado) {
            DWORD error = GetLastError();
            if (error != ERROR_TIMEOUT && g_run_bot) {
                LOG_DEBUG(_T("Erro ao ler do pipe do servidor: %lu. Desligando bot '%s'."), error, bg->bot_name);
                g_run_bot = FALSE;
                break;
            }
        }
        if (bytesLidos == 0 && resultado) {
            Sleep(50);
        }
    }
    LOG_DEBUG(_T("Thread de escuta do servidor para o bot '%s' terminando."), bg->bot_name);
    g_run_bot = FALSE;
    return 0;
}

bool can_form_word_with_letters(const TCHAR* word_candidate_uppercase, const TCHAR* available_letters_from_mp_uppercase) {
    int word_len = _tcslen(word_candidate_uppercase);
    if (word_len == 0) return false;

    TCHAR available_copy[MAXLETRAS + 1];
    _tcscpy_s(available_copy, MAXLETRAS + 1, available_letters_from_mp_uppercase);
    int available_len = _tcslen(available_copy);

    for (int i = 0; i < word_len; ++i) {
        TCHAR char_needed = word_candidate_uppercase[i];
        TCHAR* found_char_ptr = NULL;

        for (int j = 0; j < available_len; ++j) {
            if (available_copy[j] == char_needed) {
                found_char_ptr = &available_copy[j];
                break;
            }
        }

        if (found_char_ptr != NULL) {
            int index_found = found_char_ptr - available_copy;
            memmove(&available_copy[index_found], &available_copy[index_found + 1], (available_len - index_found) * sizeof(TCHAR));
            available_len--;
        }
        else {
            return false;
        }
    }
    return true;
}

DWORD WINAPI threadAdivinharPalavraComDicionario(LPVOID lpParam) {
    bot_globals* bg = (bot_globals*)lpParam;
    TCHAR letras_disponiveis_da_mp_upper[MAXLETRAS + 1];

    LOG_DEBUG(_T("Thread de adivinhar palavras (com dicionario) para '%s' iniciada."), bg->bot_name);

    if (g_NumeroDePalavrasNoDicionario == 0) {
        _tprintf(_T("[BOT_AVISO:%s]: Nenhum dicionário carregado. Thread de adivinhação não pode funcionar.\n"), bg->bot_name);
        return 1;
    }

    while (g_run_bot) {
        DWORD wait_result = WaitForSingleObject(bg->hEvento_mp, bg->reaction_time_ms / 2);

        if (!g_run_bot) break;

        if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_TIMEOUT) {
            if (bg->hMutex_mp == NULL || WaitForSingleObject(bg->hMutex_mp, INFINITE) == WAIT_OBJECT_0) {
                if (bg->hMutex_mp == NULL && _tcscmp(NOME_MUTEX, _T("")) != 0) {
                    LOG_DEBUG(_T("threadAdivinharPalavra: Mutex da MP é NULL mas esperado. Terminando."));
                    g_run_bot = FALSE;
                    break;
                }

                ZeroMemory(letras_disponiveis_da_mp_upper, sizeof(letras_disponiveis_da_mp_upper));
                int current_mp_idx = 0;
                for (int i = 0; i < MAXLETRAS; ++i) {
                    if (bg->dados_mp->letras[i] != _T('\0') && bg->dados_mp->letras[i] != _T('_')) {
                        if (current_mp_idx < MAXLETRAS) {
                            letras_disponiveis_da_mp_upper[current_mp_idx++] = _totupper(bg->dados_mp->letras[i]);
                        }
                    }
                }
                letras_disponiveis_da_mp_upper[current_mp_idx] = _T('\0');

                if (bg->hMutex_mp != NULL) ReleaseMutex(bg->hMutex_mp);

                LOG_DEBUG(_T("Bot '%s' - Letras disponiveis na MP: '%s'"), bg->bot_name, letras_disponiveis_da_mp_upper);

                if (_tcslen(letras_disponiveis_da_mp_upper) == 0) {
                    Sleep(200);
                    continue;
                }

                if (!g_run_bot) break;

                int think_time = bg->reaction_time_ms / 2 + (rand() % (bg->reaction_time_ms / 2 + 1));
                LOG_DEBUG(_T("Bot '%s' a pensar por %d ms com letras: %s"), bg->bot_name, think_time, letras_disponiveis_da_mp_upper);

                DWORD start_think_time = GetTickCount();
                while (GetTickCount() - start_think_time < (DWORD)think_time) {
                    if (!g_run_bot) break;
                    Sleep(50);
                }
                if (!g_run_bot) break;

                // Decidir se tenta um palavra errada ou procura no dicionário
                if ((rand() % 100) < CHANCE_BOT_TENTAR_PALAVRA_ERRADA && _tcslen(letras_disponiveis_da_mp_upper) >= MIN_LEN_PALAVRA_ERRADA) {
                    TCHAR palavra_errada[TAMANHO_MAX_PALAVRA_DICIONARIO] = { 0 };
                    int len_palavra = MIN_LEN_PALAVRA_ERRADA + (rand() % (MAX_LEN_PALAVRA_ERRADA - MIN_LEN_PALAVRA_ERRADA + 1));

                    // Garante que o palavra não é maior que as letras disponíveis ou o buffer
                    if (len_palavra > (int)_tcslen(letras_disponiveis_da_mp_upper)) {
                        len_palavra = _tcslen(letras_disponiveis_da_mp_upper);
                    }
                    if (len_palavra >= TAMANHO_MAX_PALAVRA_DICIONARIO) {
                        len_palavra = TAMANHO_MAX_PALAVRA_DICIONARIO - 1;
                    }
                    if (len_palavra < MIN_LEN_PALAVRA_ERRADA && _tcslen(letras_disponiveis_da_mp_upper) >= MIN_LEN_PALAVRA_ERRADA) {
                        len_palavra = MIN_LEN_PALAVRA_ERRADA;
                    }
                    if (len_palavra <= 0) {
                        LOG_DEBUG(_T("Bot '%s' não tem letras suficientes para um palavra errada."), bg->bot_name);
                    }
                    else {
                        for (int i = 0; i < len_palavra; ++i) {
                            palavra_errada[i] = letras_disponiveis_da_mp_upper[rand() % _tcslen(letras_disponiveis_da_mp_upper)];
                        }
                        palavra_errada[len_palavra] = _T('\0');

                        _tprintf(_T("[BOT:%s] Tentando palavra errada: '%s'\n"), bg->bot_name, palavra_errada);
                        if (!enviarMensagemPipeBot(bg->hPipe, palavra_errada)) {
                            LOG_DEBUG(_T("Falha ao enviar palavra errada '%s' pelo bot '%s'. Desligando."), palavra_errada, bg->bot_name);
                            g_run_bot = FALSE;
                        }
                        Sleep(bg->reaction_time_ms / 2);
                    }
                }
                else {
                    // Procurar no dicionário
                    const TCHAR* palavra_a_submeter_original = NULL;
                    int melhor_comprimento_palavra = 0;
                    TCHAR palavra_dicionario_upper[TAMANHO_MAX_PALAVRA_DICIONARIO];

                    for (int i = 0; i < g_NumeroDePalavrasNoDicionario; ++i) {
                        if (!g_run_bot) break;

                        _tcscpy_s(palavra_dicionario_upper, TAMANHO_MAX_PALAVRA_DICIONARIO, g_Dicionario[i]);
                        _tcsupr_s(palavra_dicionario_upper, TAMANHO_MAX_PALAVRA_DICIONARIO);

                        if (can_form_word_with_letters(palavra_dicionario_upper, letras_disponiveis_da_mp_upper)) {
                            int len_atual = _tcslen(palavra_dicionario_upper);
                            if (len_atual > melhor_comprimento_palavra) {
                                melhor_comprimento_palavra = len_atual;
                                palavra_a_submeter_original = g_Dicionario[i];
                            }
                        }
                    }

                    if (palavra_a_submeter_original != NULL) {
                        TCHAR palavra_final_para_submeter[TAMANHO_MAX_PALAVRA_DICIONARIO];
                        _tcscpy_s(palavra_final_para_submeter, _countof(palavra_final_para_submeter), palavra_a_submeter_original);

                        if ((rand() % 100) < CHANCE_BOT_DIGITAR_ERRADO && _tcslen(palavra_final_para_submeter) > 0) {
                            int pos_erro = rand() % _tcslen(palavra_final_para_submeter);
                            TCHAR letra_original_case = palavra_final_para_submeter[pos_erro];
                            TCHAR letra_errada_gerada;
                            do {
                                letra_errada_gerada = _T('A') + (rand() % 26);
                            } while (letra_errada_gerada == _totupper(letra_original_case));

                            if (_istlower(letra_original_case)) {
                                palavra_final_para_submeter[pos_erro] = _totlower(letra_errada_gerada);
                            }
                            else {
                                palavra_final_para_submeter[pos_erro] = letra_errada_gerada;
                            }
                            _tprintf(_T("[BOT:%s] Oops! Ia submeter '%s', mas 'digitou errado': '%s'\n"), bg->bot_name, palavra_a_submeter_original, palavra_final_para_submeter);
                        }

                        if ((rand() % 100) < CHANCE_TO_SUBMIT_WORD) {
                            _tprintf(_T("[BOT:%s] Tentando submeter: '%s'\n"), bg->bot_name, palavra_final_para_submeter);
                            if (!enviarMensagemPipeBot(bg->hPipe, palavra_final_para_submeter)) {
                                LOG_DEBUG(_T("Falha ao enviar palavra '%s' pelo bot '%s'. Desligando."), palavra_final_para_submeter, bg->bot_name);
                                g_run_bot = FALSE;
                            }
                            Sleep(bg->reaction_time_ms);
                        }
                        else {
                            LOG_DEBUG(_T("Bot '%s' (Dicionario) decidiu não submeter '%s' desta vez."), bg->bot_name, palavra_final_para_submeter);
                        }
                    }
                    else {
                        LOG_DEBUG(_T("Bot '%s' (Dicionario) não encontrou palavras formáveis com '%s'."), bg->bot_name, letras_disponiveis_da_mp_upper);
                    }
                }
            }
            else {
                LOG_DEBUG(_T("threadAdivinharPalavra para '%s': Falha ao obter mutex da MP."), bg->bot_name);
                if (g_run_bot) Sleep(100);
            }
        }
        else if (wait_result == WAIT_FAILED) {
            LOG_DEBUG(_T("threadAdivinharPalavra para '%s': Falha ao esperar pelo hEvento_mp. Erro: %lu"), bg->bot_name, GetLastError());
            g_run_bot = FALSE;
        }
    }
    LOG_DEBUG(_T("Thread de adivinhar palavras (com dicionario) para '%s' terminando."), bg->bot_name);
    return 0;
}

int setup_bot(bot_globals* bg, int argc, TCHAR* argv[]) {
    ZeroMemory(bg, sizeof(bot_globals));
    srand((unsigned int)time(NULL) + GetCurrentProcessId());

    _stprintf_s(bg->bot_name, _countof(bg->bot_name), _T("%s%d"), BOT_NAME_PREFIX, rand() % 1000);
    bg->reaction_time_ms = MIN_REACTION_MS + (rand() % (MAX_REACTION_MS - MIN_REACTION_MS + 1));

    _tprintf(_T("[BOT_INFO] Iniciando como: %s (Reação: %dms)\n"), bg->bot_name, bg->reaction_time_ms);

    if (!carregarDicionario()) {
        _tprintf(_T("[BOT_ERRO_FATAL]: Falha ao carregar o dicionário. Bot não pode funcionar sem ele.\n"));
        return 1;
    }

    if (!SetConsoleCtrlHandler(CtrlHandler_Bot, TRUE)) {
        _tprintf(_T("[BOT_ERRO] Não foi possível definir o handler da consola (Ctrl-C).\n"));
    }

    if (_tcscmp(NOME_MUTEX, _T("")) != 0) {
        bg->hMutex_mp = OpenMutex(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, NOME_MUTEX);
        if (bg->hMutex_mp == NULL) {
            _tprintf(_T("[BOT_ERRO] Falha ao abrir Mutex MP '%s' (%lu).\n"), NOME_MUTEX, GetLastError());
            return 1;
        }
    }
    else {
        bg->hMutex_mp = NULL;
    }

    bg->hEvento_mp = OpenEvent(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, NOME_EVENTO);
    if (bg->hEvento_mp == NULL) {
        _tprintf(_T("[BOT_ERRO] Falha ao abrir Evento MP '%s' (%lu).\n"), NOME_EVENTO, GetLastError());
        return 1;
    }
    bg->hMapFile_mp = OpenFileMapping(FILE_MAP_READ, FALSE, NOME_MP);
    if (bg->hMapFile_mp == NULL) {
        _tprintf(_T("[BOT_ERRO] Falha ao abrir Mapeamento MP '%s' (%lu).\n"), NOME_MP, GetLastError());
        return 1;
    }
    bg->dados_mp = (MP*)MapViewOfFile(bg->hMapFile_mp, FILE_MAP_READ, 0, 0, sizeof(MP));
    if (bg->dados_mp == NULL) {
        _tprintf(_T("[BOT_ERRO] Falha ao mapear View MP '%s' (%lu).\n"), NOME_MP, GetLastError());
        return 1;
    }
    LOG_DEBUG(_T("Bot '%s' conectado à memória partilhada com sucesso."), bg->bot_name);

    bg->hPipe = CreateFile(
        PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (bg->hPipe == INVALID_HANDLE_VALUE) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao conectar ao pipe '%s' (%lu).\n"), bg->bot_name, PIPE_NAME, GetLastError());
        return 1;
    }
    LOG_DEBUG(_T("Bot '%s' conectado ao pipe do servidor com sucesso."), bg->bot_name);

    DWORD dwMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(bg->hPipe, &dwMode, NULL, NULL)) {
        LOG_DEBUG(_T("Falha ao definir modo de mensagem do pipe para bot '%s' (%lu) - continuando."), bg->bot_name, GetLastError());
    }

    return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    bot_globals bg;
    ZeroMemory(&bg, sizeof(bot_globals));

    _tprintf(_T("[BOT_MAIN] Iniciando...\n"));

    if (setup_bot(&bg, argc, argv) != 0) {
        _tprintf(_T("[BOT_MAIN] Falha no setup. Encerrando.\n"));
        cleanup_bot(&bg);
        _tprintf(_T("[BOT_MAIN] Pressione Enter para sair.\n"));
        if (_gettchar() == EOF) {};
        return 1;
    }

    _tprintf(_T("[BOT_MAIN:%s] Setup completo. Iniciando threads de jogo.\n"), bg.bot_name);

    HANDLE hThreadComandosServidor = CreateThread(NULL, 0, threadReceberComandosServidor, &bg, 0, NULL);
    HANDLE hThreadAdivinharPalavra = CreateThread(NULL, 0, threadAdivinharPalavraComDicionario, &bg, 0, NULL);

    if (hThreadComandosServidor == NULL || hThreadAdivinharPalavra == NULL) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao criar uma ou mais threads principais.\n"), bg.bot_name);
        g_run_bot = FALSE;

        if (hThreadComandosServidor) { WaitForSingleObject(hThreadComandosServidor, 1000); CloseHandle(hThreadComandosServidor); }
        if (hThreadAdivinharPalavra) { WaitForSingleObject(hThreadAdivinharPalavra, 1000); CloseHandle(hThreadAdivinharPalavra); }
    }
    else {
        _tprintf(_T("[BOT_MAIN:%s] está a correr...\n"), bg.bot_name);

        while (g_run_bot) {
            DWORD exitCodeCmd = STILL_ACTIVE, exitCodeAdv = STILL_ACTIVE;
            BOOL cmdOk = GetExitCodeThread(hThreadComandosServidor, &exitCodeCmd);
            BOOL advOk = GetExitCodeThread(hThreadAdivinharPalavra, &exitCodeAdv);

            if ((cmdOk && exitCodeCmd != STILL_ACTIVE) || (advOk && exitCodeAdv != STILL_ACTIVE)) {
                LOG_DEBUG(_T("Uma das threads do bot '%s' terminou. CmdExit: %lu, AdvExit: %lu. Sinalizando paragem."), bg.bot_name, exitCodeCmd, exitCodeAdv);
                g_run_bot = FALSE;
            }
            Sleep(500);
        }

        _tprintf(_T("[BOT_MAIN:%s] Sinal de terminação recebido ou thread terminou. Aguardando threads...\n"), bg.bot_name);
        if (hThreadComandosServidor) WaitForSingleObject(hThreadComandosServidor, INFINITE);
        if (hThreadAdivinharPalavra) WaitForSingleObject(hThreadAdivinharPalavra, INFINITE);

        if (hThreadComandosServidor) CloseHandle(hThreadComandosServidor);
        if (hThreadAdivinharPalavra) CloseHandle(hThreadAdivinharPalavra);
    }

    cleanup_bot(&bg);
    _tprintf(_T("[BOT_MAIN:%s] Desligado.\n"), bg.bot_name);

    return 0;
}