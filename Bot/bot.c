// bot.c
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <io.h>
#include <fcntl.h>
#include <string.h> 
#include <strsafe.h>
#include "mp.h"  

//-------------------------------------------------------------------
// DEFINES GERAIS E DE DEBUG
//-------------------------------------------------------------------
//#define DEBUG_MODE 
#ifdef DEBUG_MODE
#define LOG_DEBUG(format, ...) _tprintf(_T("[BOT_DEBUG] :%s:%d: " format _T("\n")), _T(__FILE__), __LINE__, ##__VA_ARGS__)
#else
#define LOG_DEBUG(FORMAT, ...)
#endif

#define BUFFER_SIZE 512
#define BOT_NAME_PREFIX_DEFAULT _T("BotPlayer") 
#define COMANDO_DESLIGAR_CLIENTE _T("/sair") 

//-------------------------------------------------------------------
// DEFINES DO DICIONÁRIO
//-------------------------------------------------------------------
#define NOME_FICHEIRO_DICIONARIO _T("dicionario.txt")
#define MAX_PALAVRAS_DICIONARIO 1000 
#define TAMANHO_MAX_PALAVRA_DICIONARIO 30

//-------------------------------------------------------------------
// DEFINES DE COMPORTAMENTO DO BOT
//-------------------------------------------------------------------
#define MIN_REACTION_MS 3000
#define MAX_REACTION_MS 7000
#define CHANCE_TO_SUBMIT_WORD 90 
#define CHANCE_BOT_TENTAR_PALAVRA_ERRADA 30 
#define CHANCE_BOT_DIGITAR_ERRADO 10    
#define MIN_LEN_PALAVRA_ERRADA 3
#define MAX_LEN_PALAVRA_ERRADA 10 

//-------------------------------------------------------------------
// VARIÁVEIS GLOBAIS
//-------------------------------------------------------------------
volatile bool g_run_bot = TRUE;

// 1. DEFINIÇÃO DA STRUCT bot_globals
typedef struct {
    MP* dados_mp;
    HANDLE hMutex_mp;
    HANDLE hEvento_mp;
    HANDLE hMapFile_mp;
    HANDLE hPipe;
    TCHAR bot_name[50];
    int reaction_time_ms;
} bot_globals;

// 2. PONTEIRO GLOBAL PARA USO COM ATEXIT
bot_globals* g_bot_globals_for_atexit = NULL;

// 3. GLOBAIS DO DICIONÁRIO
TCHAR g_Dicionario[MAX_PALAVRAS_DICIONARIO][TAMANHO_MAX_PALAVRA_DICIONARIO];
int g_NumeroDePalavrasNoDicionario = 0;

//-------------------------------------------------------------------
// PROTÓTIPOS DE FUNÇÕES
//-------------------------------------------------------------------
BOOL carregarDicionarioBot();
void cleanup_bot(bot_globals* bg);
void cleanup_bot_wrapper(void);
BOOL WINAPI CtrlHandler_Bot(DWORD fdwCtrlType);
BOOL enviarMensagemPipeBot(HANDLE hPipe, const TCHAR* mensagem);
DWORD WINAPI threadReceberComandosServidor(LPVOID lpParam);
bool can_form_word_with_letters(const TCHAR* word_candidate_uppercase, const TCHAR* available_letters_from_mp_uppercase);
DWORD WINAPI threadAdivinharPalavraComDicionario(LPVOID lpParam);
int setup_bot(bot_globals* bg, int argc, TCHAR* argv[]);

//-------------------------------------------------------------------
// IMPLEMENTAÇÃO DAS FUNÇÕES
//-------------------------------------------------------------------

BOOL carregarDicionarioBot() {
    FILE* fp = NULL;
    errno_t err = _tfopen_s(&fp, NOME_FICHEIRO_DICIONARIO, _T("r, ccs=UTF-8"));

	LOG_DEBUG(_T("Tentando abrir dicionario '%s'."), NOME_FICHEIRO_DICIONARIO);

    if (err != 0 || fp == NULL) {
        _tprintf(TEXT("[BOT_ERRO]: Nao foi possivel abrir o ficheiro do dicionario '%s'. errno: %d\n"), NOME_FICHEIRO_DICIONARIO, err);
        LOG_DEBUG(_T("Erro ao abrir dicionario. Verifique se o ficheiro '%s' existe e tem permissao de leitura."), NOME_FICHEIRO_DICIONARIO);
        return FALSE;
    }

    g_NumeroDePalavrasNoDicionario = 0;
    TCHAR linhaBuffer[TAMANHO_MAX_PALAVRA_DICIONARIO + 2];

    while (g_NumeroDePalavrasNoDicionario < MAX_PALAVRAS_DICIONARIO &&
        _fgetts(linhaBuffer, _countof(linhaBuffer), fp) != NULL)
    {
        linhaBuffer[_tcscspn(linhaBuffer, _T("\r\n"))] = _T('\0');
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
	LOG_DEBUG(_T("Dicionario carregado com %d palavras."), g_NumeroDePalavrasNoDicionario);

    if (g_NumeroDePalavrasNoDicionario == 0) {
        _tprintf(TEXT("[BOT_AVISO]: Nenhuma palavra carregada do dicionario. O ficheiro pode estar vazio ou mal formatado.\n"));
    }
    return TRUE;
}

void cleanup_bot(bot_globals* bg) {
    // Verifica se bg e bg->bot_name são válidos antes de usar em _tprintf
    const TCHAR* botNameForLog = (bg != NULL && _tcslen(bg->bot_name) > 0) ? bg->bot_name : _T("NomeIndefinido");
    LOG_DEBUG(_T("Bot '%s': Iniciando limpeza..."), botNameForLog);

    if (bg == NULL) return; // Não há nada para limpar se bg for NULL

    if (bg->dados_mp != NULL) {
        UnmapViewOfFile(bg->dados_mp);
        bg->dados_mp = NULL;
    }
    if (bg->hMapFile_mp != NULL) {
        CloseHandle(bg->hMapFile_mp);
        bg->hMapFile_mp = NULL;
    }
    if (bg->hMutex_mp != NULL) {
        CloseHandle(bg->hMutex_mp);
        bg->hMutex_mp = NULL;
    }
    if (bg->hEvento_mp != NULL) {
        CloseHandle(bg->hEvento_mp);
        bg->hEvento_mp = NULL;
    }
    if (bg->hPipe != NULL && bg->hPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(bg->hPipe);
        CloseHandle(bg->hPipe);
        bg->hPipe = INVALID_HANDLE_VALUE;
    }
    _tprintf(_T("[BOT_INFO:%s] Recursos limpos.\n"), botNameForLog);
}

BOOL WINAPI CtrlHandler_Bot(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
        if (g_run_bot) {
            _tprintf(_T("\n[BOT] Sinal de terminação (Ctrl-C/Close) recebido. A desligar...\n"));
        }
        g_run_bot = FALSE;
        Sleep(300);
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
    }
    else {
        LOG_DEBUG(_T("Mensagem '%s' enviada com sucesso."), mensagem);
    }
    return resultado;
}

DWORD WINAPI threadReceberComandosServidor(LPVOID lpParam) {
    bot_globals* bg = (bot_globals*)lpParam;
    TCHAR buffer[BUFFER_SIZE];
    LOG_DEBUG(_T("Thread de escuta do servidor para o bot '%s' iniciada."), bg->bot_name);

    DWORD dwBytesDisponiveis = 0;

    while (g_run_bot) {
        if (bg->hPipe == INVALID_HANDLE_VALUE || bg->hPipe == NULL) {
            LOG_DEBUG(_T("threadReceberComandosServidor (%s): Pipe inválido. Saindo."), bg->bot_name);
            g_run_bot = FALSE;
            break;
        }

        BOOL peekOk = PeekNamedPipe(bg->hPipe, NULL, 0, NULL, &dwBytesDisponiveis, NULL);

        if (!g_run_bot) break;

        if (!peekOk) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                LOG_DEBUG(_T("threadReceberComandosServidor (%s): Pipe quebrado/desconectado (PeekNamedPipe Erro: %lu)."), bg->bot_name, error);
            }
            else {
                LOG_DEBUG(_T("threadReceberComandosServidor (%s): Erro ao espreitar o pipe (PeekNamedPipe Erro: %lu)."), bg->bot_name, error);
            }
            g_run_bot = FALSE;
            break;
        }

        if (dwBytesDisponiveis > 0) {
            DWORD bytesLidosNestaChamada = 0;
            DWORD bytesParaLer = min(dwBytesDisponiveis, (BUFFER_SIZE - 1) * sizeof(TCHAR));

            ZeroMemory(buffer, sizeof(buffer));
            BOOL readOk = ReadFile(bg->hPipe, buffer, bytesParaLer, &bytesLidosNestaChamada, NULL);

            if (!g_run_bot) break;

            if (readOk && bytesLidosNestaChamada > 0) {
                buffer[bytesLidosNestaChamada / sizeof(TCHAR)] = _T('\0');
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
            else if (!readOk) {
                DWORD error = GetLastError();
                LOG_DEBUG(_T("threadReceberComandosServidor (%s): Erro ao ler do pipe (ReadFile Erro: %lu)."), bg->bot_name, error);
                g_run_bot = FALSE;
                break;
            }
        }
        else {
            Sleep(100);
        }
    }

    LOG_DEBUG(_T("Thread de escuta do servidor para o bot '%s' terminando."), bg->bot_name);
    g_run_bot = FALSE;
    return 0;
}

bool can_form_word_with_letters(const TCHAR* word_candidate_uppercase, const TCHAR* available_letters_from_mp_uppercase) {
    int word_len = (int)_tcslen(word_candidate_uppercase);
    if (word_len == 0) return false;

    TCHAR available_copy[MAXLETRAS + 1];
    _tcscpy_s(available_copy, _countof(available_copy), available_letters_from_mp_uppercase);
    int available_len = (int)_tcslen(available_copy);

    for (int i = 0; i < word_len; ++i) {
        TCHAR char_needed = word_candidate_uppercase[i];
        bool found_this_char = false;
        for (int j = 0; j < available_len; ++j) {
            if (available_copy[j] == char_needed) {
                memmove(&available_copy[j], &available_copy[j + 1], (available_len - j) * sizeof(TCHAR));
                available_len--;
                found_this_char = true;
                break;
            }
        }
        if (!found_this_char) {
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
        _tprintf(_T("[BOT_AVISO:%s]: Nenhum dicionário carregado.\n"), bg->bot_name);
    }

    while (g_run_bot) {
        DWORD wait_result_evento = WaitForSingleObject(bg->hEvento_mp, bg->reaction_time_ms / 3);

        if (!g_run_bot) break;

        if (bg->hMutex_mp == NULL || WaitForSingleObject(bg->hMutex_mp, INFINITE) == WAIT_OBJECT_0) {
            ZeroMemory(letras_disponiveis_da_mp_upper, sizeof(letras_disponiveis_da_mp_upper));
            int current_mp_idx = 0;
            if (bg->dados_mp != NULL) {
                for (int i = 0; i < MAXLETRAS; ++i) {
                    if (bg->dados_mp->letras[i] != _T('\0') && bg->dados_mp->letras[i] != _T('_')) {
                        if (current_mp_idx < MAXLETRAS) {
                            letras_disponiveis_da_mp_upper[current_mp_idx++] = _totupper(bg->dados_mp->letras[i]);
                        }
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
            // LOG_DEBUG(_T("Bot '%s' a pensar por %d ms com letras: %s"), bg->bot_name, think_time, letras_disponiveis_da_mp_upper); // Log pode ser verboso

            DWORD start_think_time = GetTickCount();
            while (GetTickCount() - start_think_time < (DWORD)think_time) {
                if (!g_run_bot) break;
                Sleep(50);
            }
            if (!g_run_bot) break;

            TCHAR palavra_final_para_submeter[TAMANHO_MAX_PALAVRA_DICIONARIO] = { 0 };
            bool encontrou_palavra_valida_no_dicionario = false;
            const TCHAR* pPalavraOriginalDoDicionario = NULL;

            if (g_NumeroDePalavrasNoDicionario > 0 && (rand() % 100) >= CHANCE_BOT_TENTAR_PALAVRA_ERRADA) {
                int melhor_comprimento_palavra = 0;
                TCHAR palavra_dicionario_upper[TAMANHO_MAX_PALAVRA_DICIONARIO];
                int* indices_dicionario = malloc(g_NumeroDePalavrasNoDicionario * sizeof(int));

                if (indices_dicionario != NULL) {
                    for (int k = 0; k < g_NumeroDePalavrasNoDicionario; ++k) indices_dicionario[k] = k;
                    for (int k = g_NumeroDePalavrasNoDicionario - 1; k > 0; --k) {
                        int j_rand = rand() % (k + 1);
                        int temp_idx = indices_dicionario[k];
                        indices_dicionario[k] = indices_dicionario[j_rand];
                        indices_dicionario[j_rand] = temp_idx;
                    }
                    for (int k_idx = 0; k_idx < g_NumeroDePalavrasNoDicionario; ++k_idx) {
                        if (!g_run_bot) break;
                        int i_dic = indices_dicionario[k_idx];
                        _tcscpy_s(palavra_dicionario_upper, TAMANHO_MAX_PALAVRA_DICIONARIO, g_Dicionario[i_dic]);
                        _tcsupr_s(palavra_dicionario_upper, TAMANHO_MAX_PALAVRA_DICIONARIO);

                        if (can_form_word_with_letters(palavra_dicionario_upper, letras_disponiveis_da_mp_upper)) {
                            int len_atual = (int)_tcslen(palavra_dicionario_upper);
                            if (len_atual > melhor_comprimento_palavra) {
                                melhor_comprimento_palavra = len_atual;
                                pPalavraOriginalDoDicionario = g_Dicionario[i_dic];
                            }
                        }
                    }
                    free(indices_dicionario);
                }
                if (pPalavraOriginalDoDicionario != NULL) {
                    _tcscpy_s(palavra_final_para_submeter, _countof(palavra_final_para_submeter), pPalavraOriginalDoDicionario);
                    encontrou_palavra_valida_no_dicionario = true;
                    LOG_DEBUG(_T("Bot '%s' (Dicionario) encontrou: '%s'"), bg->bot_name, palavra_final_para_submeter);
                }
                else {
                    LOG_DEBUG(_T("Bot '%s' (Dicionario) não encontrou palavras formáveis com '%s'."), bg->bot_name, letras_disponiveis_da_mp_upper);
                }
            }

            if (!encontrou_palavra_valida_no_dicionario && _tcslen(letras_disponiveis_da_mp_upper) >= MIN_LEN_PALAVRA_ERRADA) {
                int len_palavra_errada = MIN_LEN_PALAVRA_ERRADA + (rand() % (min(MAX_LEN_PALAVRA_ERRADA, (int)_tcslen(letras_disponiveis_da_mp_upper)) - MIN_LEN_PALAVRA_ERRADA + 1));
                len_palavra_errada = min(len_palavra_errada, TAMANHO_MAX_PALAVRA_DICIONARIO - 1);

                if (len_palavra_errada > 0 && _tcslen(letras_disponiveis_da_mp_upper) > 0) {
                    for (int i = 0; i < len_palavra_errada; ++i) {
                        palavra_final_para_submeter[i] = letras_disponiveis_da_mp_upper[rand() % _tcslen(letras_disponiveis_da_mp_upper)];
                    }
                    palavra_final_para_submeter[len_palavra_errada] = _T('\0');
                    LOG_DEBUG(_T("Bot '%s' (Errada) vai tentar: '%s'"), bg->bot_name, palavra_final_para_submeter);
                }
                else {
                    LOG_DEBUG(_T("Bot '%s' (Errada) não pôde gerar palavra aleatória (letras_disponiveis: '%s', len_errada: %d)."), bg->bot_name, letras_disponiveis_da_mp_upper, len_palavra_errada);
                }
            }

            if (_tcslen(palavra_final_para_submeter) > 0) {
                if ((rand() % 100) < CHANCE_BOT_DIGITAR_ERRADO) { // Removido _tcslen > 0, já verificado acima
                    int pos_erro = rand() % (int)_tcslen(palavra_final_para_submeter);
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
                    _tprintf(_T("[BOT:%s] Oops! Preparou '%s', mas 'digitou errado': '%s'\n"), bg->bot_name,
                        (encontrou_palavra_valida_no_dicionario && pPalavraOriginalDoDicionario ? pPalavraOriginalDoDicionario : _T("palavra_aleatoria")),
                        palavra_final_para_submeter);
                }

                if ((rand() % 100) < CHANCE_TO_SUBMIT_WORD) {
                    _tprintf(_T("[BOT:%s -> Servidor]: %s\n"), bg->bot_name, palavra_final_para_submeter);
                    if (!enviarMensagemPipeBot(bg->hPipe, palavra_final_para_submeter)) {
                        LOG_DEBUG(_T("Falha ao enviar palavra '%s' pelo bot '%s'. Desligando."), palavra_final_para_submeter, bg->bot_name);
                        g_run_bot = FALSE;
                    }
                    int post_submit_wait = bg->reaction_time_ms / 2 + (rand() % (bg->reaction_time_ms / 2 + 1));
                    Sleep(post_submit_wait);
                }
                else {
                    LOG_DEBUG(_T("Bot '%s' decidiu não submeter '%s' desta vez."), bg->bot_name, palavra_final_para_submeter);
                }
            }
            else {
                LOG_DEBUG(_T("Bot '%s' não conseguiu gerar nenhuma palavra para submeter desta vez."), bg->bot_name);
            }
        }
        else if (wait_result_evento == WAIT_FAILED) {
            LOG_DEBUG(_T("threadAdivinharPalavra para '%s': Falha ao esperar pelo hEvento_mp. Erro: %lu"), bg->bot_name, GetLastError());
            g_run_bot = FALSE;
        }
        else {
            LOG_DEBUG(_T("threadAdivinharPalavra para '%s': Falha ao obter mutex da MP."), bg->bot_name);
            if (g_run_bot) Sleep(200);
        }
    }
    LOG_DEBUG(_T("Thread de adivinhar palavras (com dicionario) para '%s' terminando."), bg->bot_name);
    return 0;
}

int setup_bot(bot_globals* bg, int argc, TCHAR* argv[]) {
    ZeroMemory(bg, sizeof(bot_globals));
    srand((unsigned int)time(NULL) + GetCurrentProcessId());

    TCHAR* nomePipeServidor = NULL;
    if (argc > 1 && argv[1] != NULL && _tcslen(argv[1]) > 0) {
        nomePipeServidor = argv[1];
    }
    else {
        _tprintf(_T("[BOT_ERRO_FATAL]: Nome do pipe do servidor não fornecido como primeiro argumento.\n"));
        return 1;
    }

    if (argc > 2 && argv[2] != NULL && _tcslen(argv[2]) > 0) {
        StringCchCopy(bg->bot_name, _countof(bg->bot_name), argv[2]);
		LOG_DEBUG(_T("[BOT_INFO] Nome do bot fornecido: %s"), bg->bot_name);
    }
    else {
        _stprintf_s(bg->bot_name, _countof(bg->bot_name), _T("%s%d"), BOT_NAME_PREFIX_DEFAULT, rand() % 1000);
		LOG_DEBUG(_T("[BOT_INFO] Nome do bot não fornecido, usando padrão: %s"), bg->bot_name);
    }

    bg->reaction_time_ms = MIN_REACTION_MS + (rand() % (MAX_REACTION_MS - MIN_REACTION_MS + 1));
    if (!carregarDicionarioBot()) {
        _tprintf(_T("[BOT_ERRO_FATAL:%s]: Falha ao carregar o dicionário.\n"), bg->bot_name);
        return 1;
    }

    if (!SetConsoleCtrlHandler(CtrlHandler_Bot, TRUE)) {
        _tprintf(_T("[BOT_ERRO:%s] Não foi possível definir o handler da consola (Ctrl-C).\n"), bg->bot_name);
    }

    if (NOME_MUTEX != NULL && _tcscmp(NOME_MUTEX, _T("")) != 0) {
        bg->hMutex_mp = OpenMutex(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, NOME_MUTEX);
        if (bg->hMutex_mp == NULL) {
            _tprintf(_T("[BOT_ERRO:%s] Falha ao abrir Mutex MP '%s' (%lu).\n"), bg->bot_name, NOME_MUTEX, GetLastError());
            cleanup_bot(bg); return 1; // Chamar cleanup se falhar aqui
        }
    }
    else {
        bg->hMutex_mp = NULL;
    }

    bg->hEvento_mp = OpenEvent(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, NOME_EVENTO);
    if (bg->hEvento_mp == NULL) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao abrir Evento MP '%s' (%lu).\n"), bg->bot_name, NOME_EVENTO, GetLastError());
        cleanup_bot(bg); return 1;
    }

    bg->hMapFile_mp = OpenFileMapping(FILE_MAP_READ, FALSE, NOME_MP);
    if (bg->hMapFile_mp == NULL) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao abrir Mapeamento MP '%s' (%lu).\n"), bg->bot_name, NOME_MP, GetLastError());
        cleanup_bot(bg); return 1;
    }

    bg->dados_mp = (MP*)MapViewOfFile(bg->hMapFile_mp, FILE_MAP_READ, 0, 0, sizeof(MP));
    if (bg->dados_mp == NULL) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao mapear View MP '%s' (%lu).\n"), bg->bot_name, NOME_MP, GetLastError());
        cleanup_bot(bg); return 1;
    }
    LOG_DEBUG(_T("Bot '%s' conectado à memória partilhada com sucesso."), bg->bot_name);

    bg->hPipe = CreateFile(
        nomePipeServidor,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL); // Removido FILE_FLAG_OVERLAPPED por simplicidade, já que PeekNamedPipe é usado

    if (bg->hPipe == INVALID_HANDLE_VALUE) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao conectar ao pipe '%s' (%lu).\n"), bg->bot_name, nomePipeServidor, GetLastError());
        cleanup_bot(bg); return 1;
    }
    LOG_DEBUG(_T("Bot '%s' conectado ao pipe do servidor '%s' com sucesso."), bg->bot_name, nomePipeServidor);

    DWORD dwMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(bg->hPipe, &dwMode, NULL, NULL)) {
        LOG_DEBUG(_T("Falha ao definir modo de mensagem do pipe para bot '%s' (%lu) - continuando."), bg->bot_name, GetLastError());
    }
    return 0;
}

void cleanup_bot_wrapper(void) {
    if (g_bot_globals_for_atexit != NULL) {
        const TCHAR* botNameForLog = (_tcslen(g_bot_globals_for_atexit->bot_name) > 0) ?
            g_bot_globals_for_atexit->bot_name : _T("NomeIndefinido");
        _tprintf(_T("[BOT_ATEXIT:%s] Chamando cleanup_bot via atexit.\n"), botNameForLog);
        cleanup_bot(g_bot_globals_for_atexit);
    }
    else {
        _tprintf(_T("[BOT_ATEXIT] g_bot_globals_for_atexit é NULL, não pode limpar.\n"));
    }
}

int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    bot_globals bg_local_main;
    ZeroMemory(&bg_local_main, sizeof(bot_globals));

    g_bot_globals_for_atexit = &bg_local_main;

    if (atexit(cleanup_bot_wrapper) != 0) {
        _tprintf(_T("[BOT_AVISO] Falha ao registrar a função atexit para limpeza.\n"));
    }

    if (argc < 3) {
        _tprintf(_T("Uso: %s <NomeDoPipeServidor> <NomeDoBot>\n"), (argc > 0 ? argv[0] : _T("bot.exe")));
        _tprintf(_T("Exemplo: %s \\\\.\\pipe\\pipe BotAmigo\n"), (argc > 0 ? argv[0] : _T("bot.exe")));
        return 1;
    }

    // O setup_bot agora deve ser chamado ANTES do envio do login, pois ele conecta o pipe.
    if (setup_bot(&bg_local_main, argc, argv) != 0) {
        _tprintf(_T("[BOT_MAIN] Falha no setup. Encerrando.\n"));
        _tprintf(_T("[BOT_MAIN] Pressione Enter para sair.\n"));
        if (_gettchar() == EOF) {};
        return 1;
    }

    TCHAR loginCmd[BUFFER_SIZE];
    // Usar StringCchPrintf para segurança
    HRESULT hr = StringCchPrintf(loginCmd, _countof(loginCmd), _T("/login %s"), bg_local_main.bot_name);

    if (FAILED(hr)) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao formatar comando de login. Encerrando.\n"), bg_local_main.bot_name);
        // cleanup_bot já está registrada com atexit, então apenas retornamos.
        return 1;
    }

    if (!enviarMensagemPipeBot(bg_local_main.hPipe, loginCmd)) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao enviar comando de login para o servidor. Encerrando.\n"), bg_local_main.bot_name);
        return 1;
    }

    // Esperar pela resposta do login do servidor
    TCHAR respostaLogin[BUFFER_SIZE];
    DWORD bytesLidosLogin;

    ZeroMemory(respostaLogin, sizeof(respostaLogin));
    if (ReadFile(bg_local_main.hPipe, respostaLogin, (sizeof(respostaLogin) / sizeof(TCHAR) - 1) * sizeof(TCHAR), &bytesLidosLogin, NULL) && bytesLidosLogin > 0) {
        respostaLogin[bytesLidosLogin / sizeof(TCHAR)] = _T('\0');

        if (_tcsicmp(respostaLogin, _T("/login_ok")) != 0) {
            _tprintf(_T("[BOT_ERRO:%s] Login falhou ou foi rejeitado pelo servidor. Motivo: '%s'. Encerrando.\n"), bg_local_main.bot_name, respostaLogin);
            return 1;
        }
    }
    else {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao receber resposta de login do servidor ou pipe fechado. Erro: %lu. Encerrando.\n"), bg_local_main.bot_name, GetLastError());
        return 1;
    }

    HANDLE hThreads[2];
    hThreads[0] = CreateThread(NULL, 0, threadReceberComandosServidor, &bg_local_main, 0, NULL);
    hThreads[1] = CreateThread(NULL, 0, threadAdivinharPalavraComDicionario, &bg_local_main, 0, NULL);

    if (hThreads[0] == NULL || hThreads[1] == NULL) {
        _tprintf(_T("[BOT_ERRO:%s] Falha ao criar uma ou mais threads principais de comportamento.\n"), bg_local_main.bot_name);
        g_run_bot = FALSE;

        if (hThreads[0]) { WaitForSingleObject(hThreads[0], 2000); CloseHandle(hThreads[0]); }
        if (hThreads[1]) { WaitForSingleObject(hThreads[1], 2000); CloseHandle(hThreads[1]); }
    }
    else {
        while (g_run_bot) {
            // ... (lógica de monitoramento das threads como antes) ...
            DWORD exitCodeCmd = STILL_ACTIVE, exitCodeAdv = STILL_ACTIVE;
            BOOL cmdOk = FALSE, advOk = FALSE;

            if (hThreads[0]) cmdOk = GetExitCodeThread(hThreads[0], &exitCodeCmd);
            if (hThreads[1]) advOk = GetExitCodeThread(hThreads[1], &exitCodeAdv);

            if ((cmdOk && exitCodeCmd != STILL_ACTIVE) || (advOk && exitCodeAdv != STILL_ACTIVE)) {
                LOG_DEBUG(_T("Uma das threads do bot '%s' terminou. CmdExit: %lu (%s), AdvExit: %lu (%s). Sinalizando paragem."),
                    bg_local_main.bot_name,
                    exitCodeCmd, (cmdOk && exitCodeCmd != STILL_ACTIVE) ? _T("terminou") : _T("ativa"),
                    exitCodeAdv, (advOk && exitCodeAdv != STILL_ACTIVE) ? _T("terminou") : _T("ativa"));
                g_run_bot = FALSE;
            }
            Sleep(500);
        }

        _tprintf(_T("[BOT_MAIN:%s] Sinal de terminação recebido ou thread terminou. Aguardando threads de comportamento...\n"), bg_local_main.bot_name);
        g_run_bot = FALSE;

        if (hThreads[0]) WaitForSingleObject(hThreads[0], INFINITE);
        if (hThreads[1]) WaitForSingleObject(hThreads[1], INFINITE);

        if (hThreads[0]) CloseHandle(hThreads[0]);
        if (hThreads[1]) CloseHandle(hThreads[1]);
        _tprintf(_T("[BOT_MAIN:%s] Threads de comportamento terminadas.\n"), bg_local_main.bot_name);
    }

    _tprintf(_T("[BOT_MAIN:%s] Encerrando.\n"), bg_local_main.bot_name);
    // cleanup_bot_wrapper será chamado por atexit.

    return 0;
}