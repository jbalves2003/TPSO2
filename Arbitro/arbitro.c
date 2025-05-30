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

#define NOME_EXECUTAVEL_BOT _T("bot.exe")
#define MOME_FICHEIRO_DICIONARIO _T("dicionario.txt")
#define MAX_PALAVRAS 1000
#define TAMAMANHO_MAX_PALAVRA 30
TCHAR g_Dicionario[MAX_PALAVRAS][TAMAMANHO_MAX_PALAVRA];
int g_NumeroDePalavrasNoDicionario = 0;

#define PIPE_NAME _T("\\\\.\\pipe\\pipe")
#define BUFFER_SIZE 512

/////////////////////////////////registry importante ////////////////////////////////////
#define TEMPO 1000 
int ritmo = 5 * TEMPO;

#define PONTUACAO_GANHA 2 // Pontuação por palavra correta
#define PONTUACAO_PERDIDA 1 // Pontuação perdida por palavra errada
#define MIN_LETRAS_PARA_PONTUAR 2 // Mínimo de letras usadas para pontuar

#define NAME_SIZE 20
#define MAXJOGADORES 20
#define NOME_MUTEX_JOGADORES NULL // Mutex para proteger acesso à lista de jogadores

#define COMANDO_DESLIGAR_CLIENTE _T("/sair")
#define PROMPT_INPUT _T("Arbitro> ")

volatile bool run = TRUE;

typedef struct {
    MP* dados;
    HANDLE hMutex;
    HANDLE hEvento;

    HANDLE mapFile;
} memoria_partilhada;

typedef struct {
    HANDLE hPipeCliente;
    DWORD dwThreadIdCliente;
    int id_jogador;
    TCHAR nome[NAME_SIZE];
    int pontos;
} jogador;

typedef struct {
    jogador jogadores[MAXJOGADORES];
    int num_jogadores;
    int prox_id_jogador;
    HANDLE g_hMutexJogadores;
} lista_jogadores;

typedef struct {
    memoria_partilhada* mp;
    lista_jogadores* listaJogadores;
    HANDLE hConsoleOutputMutex;
    TCHAR adminInputLine[BUFFER_SIZE];
    int adminInputCursorPos;
} globais;

typedef struct {
    HANDLE hPipeCliente;
    int id_jogador;
    TCHAR nome[NAME_SIZE];
    int pontos;

    globais* g;
}clienteAux;

typedef enum {
    invalido_jogador = -1,
    sair,
    jogs,
    pont,
    palavra
} comando_jogador;

comando_jogador checkComandoJogador(const TCHAR* comando) {
    if (comando == NULL) return invalido_jogador;

    if (_tcsicmp(comando, COMANDO_DESLIGAR_CLIENTE) == 0) return sair;
    if (_tcsicmp(comando, _T("/jogs")) == 0) return jogs;
    if (_tcsicmp(comando, _T("/pont")) == 0) return pont;
    return palavra;
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

void escreverOutput(globais* g, const TCHAR* format, ...) {
    if (g == NULL || g->hConsoleOutputMutex == NULL) {
        va_list args_fallback;
        va_start(args_fallback, format);
        _vtprintf(format, args_fallback);
        va_end(args_fallback);
        _tprintf(_T("\n"));
        fflush(stdout);
        return;
    }

    TCHAR message_buffer[2048]; // Buffer para a mensagem a ser impressa
    va_list args;
    va_start(args, format);
    StringCchVPrintf(message_buffer, _countof(message_buffer), format, args);
    va_end(args);

    if (WaitForSingleObject(g->hConsoleOutputMutex, INFINITE) == WAIT_OBJECT_0) {
        _tprintf(_T("\r%*s\r"), 80, _T(""));

        _tprintf(_T("%s\n"), message_buffer);

        _tprintf(_T("Arbitro> %s"), g->adminInputLine); // Se adminInputLine está vazio, só imprime "Arbitro> "

        fflush(stdout);
        ReleaseMutex(g->hConsoleOutputMutex);
    }
}

BOOL carregarDicionario() {
    FILE* fp = NULL;
    errno_t err = _tfopen_s(&fp, MOME_FICHEIRO_DICIONARIO, _T("r, ccs=UTF-8"));

    if (err != 0 || fp == NULL) {
        _tprintf(TEXT("ARBITRO ERRO: Nao foi possivel abrir o ficheiro do dicionario '%s'. errno: %d\n"), MOME_FICHEIRO_DICIONARIO, err);
        perror("Detalhe do erro de _tfopen_s");
        return FALSE;
    }

    LOG_DEBUG(_T("carregarDicionario: Ficheiro '%s' aberto com sucesso."), MOME_FICHEIRO_DICIONARIO);
    g_NumeroDePalavrasNoDicionario = 0;
    TCHAR linhaBuffer[TAMAMANHO_MAX_PALAVRA + 2];

    while (g_NumeroDePalavrasNoDicionario < MAX_PALAVRAS &&
        _fgetts(linhaBuffer, _countof(linhaBuffer), fp) != NULL)
    {
        linhaBuffer[_tcscspn(linhaBuffer, _T("\r\n"))] = _T('\0');

        if (_tcslen(linhaBuffer) > 0 && _tcslen(linhaBuffer) < TAMAMANHO_MAX_PALAVRA) { // Ignorar linhas vazias e muito longas
            _tcscpy_s(g_Dicionario[g_NumeroDePalavrasNoDicionario], TAMAMANHO_MAX_PALAVRA, linhaBuffer);
            g_NumeroDePalavrasNoDicionario++;
        }
    }

    fclose(fp);
    LOG_DEBUG(_T("carregarDicionario: Carregadas %d palavras do dicionario."), g_NumeroDePalavrasNoDicionario);

    if (g_NumeroDePalavrasNoDicionario == 0) {
        _tprintf(TEXT("ARBITRO ERRO: O dicionário está vazio ou não contém palavras válidas.\n"));
        return FALSE;
    }
    return TRUE;
}

BOOL palavraExisteNoDicionario(const TCHAR* palavra) {
    if (palavra == NULL || g_NumeroDePalavrasNoDicionario == 0) return FALSE;

    TCHAR palavraFormatada[TAMAMANHO_MAX_PALAVRA];
    _tcscpy_s(palavraFormatada, _countof(palavraFormatada), palavra);

    for (int i = 0; i < g_NumeroDePalavrasNoDicionario; ++i)
        if (_tcsicmp(palavraFormatada, g_Dicionario[i]) == 0)
            return TRUE;

    return FALSE;
}

void enviarMensagemPipe(HANDLE hPipe, const TCHAR* mensagem) {
    if (hPipe == NULL || mensagem == NULL) return;

    DWORD bytesEscritos;
    BOOL resultado = WriteFile(hPipe, mensagem, (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR), &bytesEscritos, NULL);

    if (!resultado || bytesEscritos != (DWORD)(_tcslen(mensagem) + 1) * sizeof(TCHAR))
        LOG_DEBUG(_T("Erro ao enviar comando '%s' pelo pipe: %lu\n"), mensagem, GetLastError());
}

TCHAR* listarJogadores(lista_jogadores* lista) {
    TCHAR* buffer = (TCHAR*)malloc(BUFFER_SIZE * sizeof(TCHAR));
    if (buffer == NULL) return NULL;
    buffer[0] = _T('\0');

    TCHAR* pBuffer = buffer;

    if (lista == NULL || lista->g_hMutexJogadores == NULL) {
        _tcscpy_s(buffer, BUFFER_SIZE, _T("Erro: Lista de jogadores inválida.\n"));
        return buffer;
    }

    if (WaitForSingleObject(lista->g_hMutexJogadores, INFINITE) == WAIT_OBJECT_0) {
        if (lista->num_jogadores == 0)
            _tcscpy_s(pBuffer, BUFFER_SIZE - (pBuffer - buffer), _T("Nenhum jogador conectado.\n"));
        else {
            int charsEscritos = _stprintf_s(pBuffer, BUFFER_SIZE - (pBuffer - buffer),
                _T("Jogadores (ID Nome : Pontos):\n"));
            if (charsEscritos > 0) pBuffer += charsEscritos;

            for (int i = 0; i < lista->num_jogadores; i++) {
                size_t espacoRestante = BUFFER_SIZE - (pBuffer - buffer);
                if (espacoRestante < (NAME_SIZE + 20)) {
                    if (espacoRestante > 25) _tcscat_s(pBuffer, espacoRestante, _T("...lista truncada...\n"));
                    break;
                }

                charsEscritos = _stprintf_s(
                    pBuffer,
                    espacoRestante,
                    _T("%-5d %-*s : %8d\n"),
                    lista->jogadores[i].id_jogador,
                    NAME_SIZE,
                    lista->jogadores[i].nome,
                    lista->jogadores[i].pontos
                );

                if (charsEscritos > 0)
                    pBuffer += charsEscritos;
                else
                    break;
            }
        }
        ReleaseMutex(lista->g_hMutexJogadores);
    }
    else
        _tcscpy_s(buffer, BUFFER_SIZE, _T("Erro ao aceder lista de jogadores.\n"));

    return buffer;
}

BOOL removerJogador(lista_jogadores* lista, int id_jogador) {
    if (id_jogador < 0) return FALSE;
    LOG_DEBUG(_T("removerJogador: ID do jogador %d.\n"), id_jogador);

    if (WaitForSingleObject(lista->g_hMutexJogadores, INFINITE) != WAIT_OBJECT_0) {
        LOG_DEBUG(_T("removerJogador: ERRO ao aceder lista de jogadores.\n"));
        return FALSE;
    }
    if (lista->num_jogadores <= 0) {
        ReleaseMutex(lista->g_hMutexJogadores);
        return FALSE;
    }

    for (int i = 0; i < lista->num_jogadores; i++) {
        if (lista->jogadores[i].id_jogador == id_jogador) {
            enviarMensagemPipe(lista->jogadores[i].hPipeCliente, COMANDO_DESLIGAR_CLIENTE);
            if (i < lista->num_jogadores - 1) {
                lista->jogadores[i] = lista->jogadores[lista->num_jogadores - 1];
            }


            lista->num_jogadores--;
            ZeroMemory(&lista->jogadores[lista->num_jogadores], sizeof(jogador));

            ReleaseMutex(lista->g_hMutexJogadores);
            LOG_DEBUG(_T("removerJogador: Jogador ID %d removido com sucesso. num_jogadores agora é %d.\n"), id_jogador, lista->num_jogadores);

            return TRUE;
        }
    }

    ReleaseMutex(lista->g_hMutexJogadores);
    LOG_DEBUG(_T("removerJogador: Jogador %d removido com sucesso.\n"), id_jogador);
    return FALSE;
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
    globais* g = (globais*)lpParam;
    if (g == NULL || g->mp == NULL || g->mp->dados == NULL || g->mp->hMutex == NULL) {
        LOG_DEBUG(_T("threadGeradorLetras: Parâmetros inválidos (g, mp, mp->dados ou mp->hMutex é NULL). Terminando."));
        return 1;
    }
    memoria_partilhada* mp_local = g->mp;

    LOG_DEBUG(_T("Thread Gerador de Letras iniciada."));

    TCHAR geradorDisplayBuffer[MAXLETRAS * 3 + 30];

    while (run) {
        if (WaitForSingleObject(mp_local->hMutex, INFINITE) == WAIT_OBJECT_0) {
            if (verificaVetorVazio(mp_local->dados->letras)) {
                escreveVetor(mp_local->dados->letras);
            }
            else if (!verificaVetorVazio(mp_local->dados->letras)) {
                for (int i = MAXLETRAS - 1; i >= 0; i--) {
                    if (mp_local->dados->letras[i] != _T('\0')) {
                        apagaLetra(mp_local->dados->letras, i);
                        break;
                    }
                }
            }

            // Construir a string de display para escreverOutput
            StringCchCopy(geradorDisplayBuffer, _countof(geradorDisplayBuffer), _T("Gerador: "));
            for (int i = 0; i < MAXLETRAS; i++) {
                TCHAR letraStr[3]; // Para "X " ou "_ "
                if (mp_local->dados->letras[i] != _T('\0')) {
                    StringCchPrintf(letraStr, _countof(letraStr), _T("%c "), mp_local->dados->letras[i]);
                }
                else {
                    StringCchCopy(letraStr, _countof(letraStr), _T("_ "));
                }
                StringCchCat(geradorDisplayBuffer, _countof(geradorDisplayBuffer), letraStr);
            }

            // Remover o último espaço se houver
            size_t len = _tcslen(geradorDisplayBuffer);
            if (len > 0 && geradorDisplayBuffer[len - 1] == _T(' '))
                geradorDisplayBuffer[len - 1] = _T('\0');

            ReleaseMutex(mp_local->hMutex);

            escreverOutput(g, _T("%s"), geradorDisplayBuffer);
            if (mp_local->hEvento != NULL) { SetEvent(mp_local->hEvento); ResetEvent(mp_local->hEvento); }
        }
        else {
            LOG_DEBUG(_T("threadGeradorLetras: ERRO ao aceder ao mutex da memória partilhada."));
            run = false;
            break;
        }

        DWORD startTime = GetTickCount();
        while (GetTickCount() - startTime < ritmo) {
            if (!run) break;
            Sleep(100);
        }
        if (!run) break;
    }

    LOG_DEBUG(_T("Thread Gerador de Letras terminando."));
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

bool checkPalavraValidaEProcessar(const TCHAR* palavra_jogador, clienteAux* aux, int* pontos_ganhos) {
    *pontos_ganhos = 0;
    if (palavra_jogador == NULL || aux == NULL || aux->g == NULL || aux->g->mp == NULL || pontos_ganhos == NULL) {
        LOG_DEBUG(_T("checkPalavraValidaEProcessar: Parâmetros inválidos."));
        return false;
    }

    if (!palavraExisteNoDicionario(palavra_jogador)) {
        LOG_DEBUG(_T("checkPalavraValidaEProcessar: Palavra '%s' não encontrada no dicionário."), palavra_jogador);
        return false;
    }

    if (WaitForSingleObject(aux->g->mp->hMutex, INFINITE) != WAIT_OBJECT_0) {
        LOG_DEBUG(_T("checkPalavraValidaEProcessar: ERRO ao aceder mutex mp para palavra '%s' de %s."), palavra_jogador, aux->nome);
        return false;
    }

    bool palavra_valida = false;

    TCHAR palavra_jogador_maiuscula[TAMAMANHO_MAX_PALAVRA];
    StringCchCopy(palavra_jogador_maiuscula, _countof(palavra_jogador_maiuscula), palavra_jogador);
    _tcsupr_s(palavra_jogador_maiuscula, _countof(palavra_jogador_maiuscula));

    size_t len_palavra = _tcslen(palavra_jogador_maiuscula);

    if (len_palavra == 0 || len_palavra >= TAMAMANHO_MAX_PALAVRA) {
        ReleaseMutex(aux->g->mp->hMutex);
        LOG_DEBUG(_T("checkPalavraValidaEProcessar: Palavra '%s' tem tamanho inválido (%zu)."), palavra_jogador, len_palavra);
        return false;
    }

    TCHAR letras_disponiveis[MAXLETRAS];
    int indices_letras_removidas[MAXLETRAS];
    int num_letras_usadas = 0;

    for (int k = 0; k < MAXLETRAS; ++k)
        letras_disponiveis[k] = aux->g->mp->dados->letras[k];

    for (size_t i_palavra = 0; i_palavra < len_palavra; ++i_palavra) {
        TCHAR char_da_palavra = palavra_jogador_maiuscula[i_palavra];

        for (int j_vetor = 0; j_vetor < MAXLETRAS; ++j_vetor) {
            if (letras_disponiveis[j_vetor] == char_da_palavra &&
                letras_disponiveis[j_vetor] != _T('\0') &&
                letras_disponiveis[j_vetor] != _T('\1')) {

                indices_letras_removidas[num_letras_usadas] = j_vetor;

                letras_disponiveis[j_vetor] = _T('\1');
                num_letras_usadas++;

                LOG_DEBUG(_T("checkPalavraValidaEProcessar: Letra '%c' da palavra '%s' ENCONTRADA no vetor (índice original %d). Total encontradas: %d"),
                    char_da_palavra, palavra_jogador_maiuscula, j_vetor, num_letras_usadas);
                break;
            }
        }
    }

    if (num_letras_usadas >= MIN_LETRAS_PARA_PONTUAR) {
        int pontos_a_ganhar = num_letras_usadas * PONTUACAO_GANHA;

        aux->pontos += pontos_a_ganhar;
        *pontos_ganhos = pontos_a_ganhar;

        TCHAR letras_removidas[MAXLETRAS + 1] = { 0 };
        int current_log_idx = 0;

        for (int k = 0; k < num_letras_usadas; ++k) {
            int indice_original_para_remover = indices_letras_removidas[k];

            if (aux->g->mp->dados->letras[indice_original_para_remover] != _T('\0'))
                letras_removidas[current_log_idx++] = aux->g->mp->dados->letras[indice_original_para_remover];

            apagaLetra(aux->g->mp->dados->letras, indice_original_para_remover);
        }
        SetEvent(aux->g->mp->hEvento);

        LOG_DEBUG(_T("checkPalavraValidaEProcessar: Palavra '%s' VÁLIDA para %s. Usou %d letras do vetor ('%s'). +%d pts. Total agora: %d"), palavra_jogador, aux->nome, num_letras_usadas, letras_removidas, pontos_a_ganhar, aux->pontos);
        palavra_valida = true;
    }
    else
        LOG_DEBUG(_T("checkPalavraValidaEProcessar: Palavra '%s' de %s encontrou apenas %d letras no vetor (mínimo %d). INVÁLIDA para pontuação."), palavra_jogador, aux->nome, num_letras_usadas, MIN_LETRAS_PARA_PONTUAR);

    ReleaseMutex(aux->g->mp->hMutex);
    return palavra_valida;
}

BOOL loginCliente(clienteAux* aux) {
    if (aux == NULL || aux->g == NULL || aux->hPipeCliente == INVALID_HANDLE_VALUE) {
        LOG_DEBUG(_T("loginCliente: Parâmetros inválidos."));
        return FALSE;
    }

    TCHAR buffer[BUFFER_SIZE];
    DWORD bytesLidos;
    BOOL loginSucesso = FALSE;

    LOG_DEBUG(_T("loginCliente (pipe %p): Aguardando dados de login do cliente..."), aux->hPipeCliente);

    ZeroMemory(buffer, sizeof(buffer));
    if (ReadFile(aux->hPipeCliente, buffer, (sizeof(buffer) / sizeof(TCHAR) - 1) * sizeof(TCHAR), &bytesLidos, NULL)) {
        if (bytesLidos > 0) {
            buffer[bytesLidos / sizeof(TCHAR)] = _T('\0');
            LOG_DEBUG(_T("loginCliente (pipe %p): Recebido para login: '%s'"), aux->hPipeCliente, buffer);

            TCHAR* comando = NULL;
            TCHAR* nome = NULL;
            TCHAR* pContext = NULL;

            comando = _tcstok_s(buffer, _T(" "), &pContext);
            if (comando != NULL) {
                nome = _tcstok_s(NULL, _T(" \r\n"), &pContext);
            }

            if (comando != NULL && _tcsicmp(comando, _T("/login")) == 0 &&
                nome != NULL && _tcslen(nome) > 0 && _tcslen(nome) < NAME_SIZE) {

                if (WaitForSingleObject(aux->g->listaJogadores->g_hMutexJogadores, INFINITE) == WAIT_OBJECT_0) {
                    bool nome_ja_existe = false;
                    for (int i = 0; i < aux->g->listaJogadores->num_jogadores; ++i) {
                        if (_tcsicmp(aux->g->listaJogadores->jogadores[i].nome, nome) == 0) {
                            nome_ja_existe = true;
                            break;
                        }
                    }

                    if (aux->g->listaJogadores->num_jogadores < MAXJOGADORES && !nome_ja_existe) {
                        aux->id_jogador = aux->g->listaJogadores->prox_id_jogador;
                        StringCchCopy(aux->nome, NAME_SIZE, nome);
                        aux->pontos = 0;

                        jogador* pJogadorNaLista = &aux->g->listaJogadores->jogadores[aux->g->listaJogadores->num_jogadores];
                        pJogadorNaLista->id_jogador = aux->id_jogador;
                        StringCchCopy(pJogadorNaLista->nome, NAME_SIZE, aux->nome);
                        pJogadorNaLista->pontos = aux->pontos;
                        pJogadorNaLista->hPipeCliente = aux->hPipeCliente;
                        pJogadorNaLista->dwThreadIdCliente = GetCurrentThreadId();

                        aux->g->listaJogadores->num_jogadores++;
                        aux->g->listaJogadores->prox_id_jogador++;

                        ReleaseMutex(aux->g->listaJogadores->g_hMutexJogadores);

                        enviarMensagemPipe(aux->hPipeCliente, _T("/login_ok"));
                        loginSucesso = TRUE;
                        escreverOutput(aux->g, _T("Cliente '%s' (ID: %d, Pipe: %p) logado com sucesso. Jogadores: %d"),
                            aux->nome, aux->id_jogador, aux->hPipeCliente, aux->g->listaJogadores->num_jogadores);
                    }
                    else {
                        ReleaseMutex(aux->g->listaJogadores->g_hMutexJogadores);
                        TCHAR msgFalha[128];
                        if (nome_ja_existe) {
                            StringCchPrintf(msgFalha, _countof(msgFalha), _T("/login_fail Nome '%s' ja em uso."), nome);
                            escreverOutput(aux->g, _T("Falha no login (pipe %p): Nome '%s' já em uso."), aux->hPipeCliente, nome);
                        }
                        else { // Servidor cheio
                            StringCchCopy(msgFalha, _countof(msgFalha), _T("/login_fail Servidor cheio."));
                            escreverOutput(aux->g, _T("Falha no login (pipe %p): Servidor cheio."), aux->hPipeCliente);
                        }
                        enviarMensagemPipe(aux->hPipeCliente, msgFalha);
                    }
                }
                else {
                    LOG_DEBUG(_T("loginCliente (pipe %p): Falha ao obter mutex da lista de jogadores."), aux->hPipeCliente);
                    enviarMensagemPipe(aux->hPipeCliente, _T("/login_fail ErroInternoServidor"));
                }
            }
            else { // Comando de login inválido
                LOG_DEBUG(_T("loginCliente (pipe %p): Comando de login inválido ou nome ausente: '%s'"), aux->hPipeCliente, buffer);
                enviarMensagemPipe(aux->hPipeCliente, _T("/login_fail FormatoInvalido"));
            }
        }
        else
            LOG_DEBUG(_T("loginCliente (pipe %p): Nenhum dado de login recebido ou cliente desconectou."), aux->hPipeCliente);
    }
    else { // ReadFile falhou
        DWORD dwError = GetLastError();
        LOG_DEBUG(_T("loginCliente (pipe %p): Erro ao ler dados para login: %lu."), aux->hPipeCliente, dwError);
    }
    return loginSucesso;
}

DWORD WINAPI receberComandos(LPVOID lpParam) {
    clienteAux* aux = (clienteAux*)lpParam;
    if (aux == NULL) return 1;

    BOOL clienteLogin = FALSE;

    if (run)
        clienteLogin = loginCliente(aux);

    if (!run || !clienteLogin) {
        LOG_DEBUG(_T("Thread de cliente (Pipe %p): Login falhou ou servidor desligando. Terminando."), aux->hPipeCliente);
        if (aux->hPipeCliente != NULL && aux->hPipeCliente != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(aux->hPipeCliente);
            CloseHandle(aux->hPipeCliente);
            aux->hPipeCliente = INVALID_HANDLE_VALUE;
        }
        free(aux);
        return 1;
    }

    LOG_DEBUG(_T("Thread de recebimento de comandos iniciada para o cliente %s (ID: %d, Pipe: %p)."), aux->nome, aux->id_jogador, aux->hPipeCliente);

    HANDLE hPipeCliente = aux->hPipeCliente;
    BOOL clienteConectado = TRUE;
    TCHAR buffer[BUFFER_SIZE];

    while (run && clienteConectado) {
        if (!run) { clienteConectado = FALSE; continue; }
        DWORD dwBytesDisponiveisNoPipe = 0;

        BOOL peekOk = PeekNamedPipe(
            hPipeCliente,
            NULL,                       // buffer (não queremos ler aqui)
            0,                          // tamanho do buffer (0 porque só estamos a espreitar)
            NULL,                       // ponteiro para bytes espreitados nesta chamada (não precisamos)
            &dwBytesDisponiveisNoPipe,  // ponteiro para o total de bytes disponíveis NO PIPE
            NULL                        // ponteiro para bytes restantes nesta mensagem (para PIPE_TYPE_MESSAGE, pode ser igual a dwBytesDisponiveisNoPipe)
        );

        if (!peekOk) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
                LOG_DEBUG(_T("Jogo (%s): Pipe quebrado/desconectado (PeekNamedPipe Erro: %lu)."), aux->nome, error);
            else
                LOG_DEBUG(_T("Jogo (%s): Erro ao espreitar pipe (PeekNamedPipe Erro: %lu). Desconectando."), aux->nome, error);
            clienteConectado = FALSE;
            break;
        }

        if (dwBytesDisponiveisNoPipe > 0) {
            DWORD bytesLidos = 0;
            BOOL resultado = ReadFile(hPipeCliente, buffer, (sizeof(buffer) / sizeof(TCHAR) - 1) * sizeof(TCHAR), &bytesLidos, NULL);

            if (!run) { clienteConectado = FALSE; continue; }

            if (resultado && bytesLidos > 0) {
                buffer[bytesLidos / sizeof(TCHAR)] = _T('\0');
                LOG_DEBUG(_T("receberComandos: (Thread %lu, Pipe %p): Comando recebido do cliente '%s': %s"), GetCurrentThreadId(), hPipeCliente, aux->nome, buffer);
                comando_jogador cmd = checkComandoJogador(buffer);
                TCHAR msg[BUFFER_SIZE];

                switch (cmd)
                {
                case sair:
                    LOG_DEBUG(_T("receberComandos: Cliente '%s' solicitou desconexão."), aux->nome);
                    //enviarMensagemPipe(hPipeCliente, COMANDO_DESLIGAR_CLIENTE);
                    removerJogador(aux->g->listaJogadores, aux->id_jogador);
                    clienteConectado = FALSE;
                    escreverOutput(aux->g, _T("Cliente '%s' (ID: %d) desconectado. Total de jogadores: %d."), aux->nome, aux->id_jogador, aux->g->listaJogadores->num_jogadores);
                    break;
                case jogs:
                    LOG_DEBUG(_T("receberComandos: Cliente '%s' solicitou lista de jogadores."), aux->nome);
                    TCHAR* listaJogadores = listarJogadores(aux->g->listaJogadores);
                    enviarMensagemPipe(hPipeCliente, listaJogadores);
                    free(listaJogadores);
                    break;
                case pont:
                    LOG_DEBUG(_T("receberComandos: Cliente '%s' solicitou sua pontuação."), aux->nome);
                    _stprintf_s(msg, _countof(msg), _T("Sua pontuação atual: %d"), aux->pontos);
                    enviarMensagemPipe(hPipeCliente, msg);
                    break;
                case palavra:
                    LOG_DEBUG(_T("receberComandos: Cliente '%s' (ID %d) enviou uma palavra: '%s'"), aux->nome, aux->id_jogador, buffer);

                    int pontos_ganhos = 0;
                    bool palavra_foi_valida = checkPalavraValidaEProcessar(buffer, aux, &pontos_ganhos);

                    if (palavra_foi_valida) {
                        _stprintf_s(msg, _countof(msg), _T("/ok Palavra '%s' correta! +%d pontos."), buffer, pontos_ganhos);
                        enviarMensagemPipe(hPipeCliente, msg);
                        escreverOutput(aux->g, _T("Palavra '%s' correta para %s! +%d pts. Total atual: %d"), buffer, aux->nome, pontos_ganhos, aux->pontos);
                    }
                    else {
                        aux->pontos -= PONTUACAO_PERDIDA;
                        _stprintf_s(msg, _countof(msg), _T("/erro PalavraInvalidaOuNaoEncontrada -%d pts."), PONTUACAO_PERDIDA);
                        enviarMensagemPipe(hPipeCliente, msg);
                        LOG_DEBUG(_T("receberComandos: Palavra '%s' INVÁLIDA ou ERRO p/ %s. -%d pts. Total atual: %d"), buffer, aux->nome, PONTUACAO_PERDIDA, aux->pontos);
                    }

                    // Atualizar pontuação global do jogador
                    if (WaitForSingleObject(aux->g->listaJogadores->g_hMutexJogadores, INFINITE) == WAIT_OBJECT_0) {
                        for (int i = 0; i < aux->g->listaJogadores->num_jogadores; i++) {
                            if (aux->g->listaJogadores->jogadores[i].id_jogador == aux->id_jogador) {
                                aux->g->listaJogadores->jogadores[i].pontos = aux->pontos;
                                break;
                            }
                        }
                        ReleaseMutex(aux->g->listaJogadores->g_hMutexJogadores);
                    }
                    else {
                        LOG_DEBUG(_T("receberComandos: ERRO ao aceder mutex listaJogadores para atualizar pontos de %s."), aux->nome);
                    }
                    break;
                default:
                    LOG_DEBUG(_T("receberComandos: Comando inválido recebido do cliente '%s': %s"), aux->nome, buffer);
                    enviarMensagemPipe(hPipeCliente, _T("ComandoInvalido"));
                    break;
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
    }

    enviarMensagemPipe(hPipeCliente, COMANDO_DESLIGAR_CLIENTE);

    if (hPipeCliente != NULL && hPipeCliente != INVALID_HANDLE_VALUE) {
        LOG_DEBUG(_T("receberComandos: (Thread %lu, Pipe %p): Desconectando cliente '%s'."),
            GetCurrentThreadId(), hPipeCliente, aux->nome);
        FlushFileBuffers(hPipeCliente);
        DisconnectNamedPipe(hPipeCliente);
        CloseHandle(hPipeCliente);
    }

    free(aux);
    return 0;
}

DWORD WINAPI threadGereCliente(LPVOID lpParam) {
    globais* g = (globais*)lpParam;
    LOG_DEBUG(_T("Thread de gerenciamento de clientes iniciada."));

    while (run) {
        HANDLE hPipe = INVALID_HANDLE_VALUE;

        hPipe = CriarPipeServidorDuplex();
        if (hPipe == INVALID_HANDLE_VALUE) continue;

        BOOL conexao = ConnectNamedPipe(hPipe, NULL);
        if (!run) break;

        if (!conexao && GetLastError() != ERROR_PIPE_CONNECTED) {
            LOG_DEBUG(_T("threadGereCliente ERRO: Falha ao conectar ao pipe (%lu)."), GetLastError());
            CloseHandle(hPipe);
            continue;
        }

        clienteAux* aux = (clienteAux*)malloc(sizeof(clienteAux));
        if (aux == NULL) {
            LOG_DEBUG(_T("threadGereCliente ERRO: Falha ao alocar memória para clienteAux."));
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }
        ZeroMemory(aux, sizeof(clienteAux));

        aux->hPipeCliente = hPipe;
        aux->g = g;

        HANDLE  hThreadCliente = CreateThread(NULL, 0, receberComandos, aux, 0, NULL);

        if (hThreadCliente == NULL) {
            LOG_DEBUG(_T("threadGereCliente ERRO: Falha ao criar thread para cliente (%lu)."), GetLastError());
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            free(aux);
        }
        else
            CloseHandle(hThreadCliente);
    } // fim do while run
    return 0;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (run) _tprintf(_T("\nSinal de terminação recebido. A desligar...\n"));
        run = false;
        return TRUE;
    default:
        return FALSE;
    }
}

DWORD WINAPI threadAdminConsole(LPVOID lpParam) {
    globais* g = (globais*)lpParam;
    if (g == NULL || g->hConsoleOutputMutex == NULL) {
        LOG_DEBUG(_T("threadAdminConsole: globais ou hConsoleOutputMutex é NULL. Terminando thread."));
        return 1;
    }

    TCHAR cmdInputBuffer[BUFFER_SIZE];
    TCHAR prompt[] = PROMPT_INPUT;
    TCHAR* pContext = NULL;

    escreverOutput(g, _T("Console do Administrador iniciado. Digite '/encerrar' para desligar."));

    if (WaitForSingleObject(g->hConsoleOutputMutex, INFINITE) == WAIT_OBJECT_0) {
        _tprintf(prompt);
        fflush(stdout);
        ZeroMemory(g->adminInputLine, sizeof(g->adminInputLine));
        ReleaseMutex(g->hConsoleOutputMutex);
    }
    else {
        LOG_DEBUG(_T("threadAdminConsole: Falha crítica ao obter hConsoleOutputMutex para o prompt inicial."));
        _tprintf(prompt);
        fflush(stdout);
    }


    while (run) {
        if (_fgetts(cmdInputBuffer, _countof(cmdInputBuffer), stdin) == NULL) {
            if (run) {
                LOG_DEBUG(_T("threadAdminConsole: _fgetts retornou NULL."));
            }
            break;
        }

        cmdInputBuffer[_tcscspn(cmdInputBuffer, _T("\r\n"))] = _T('\0');

        if (!run) break;
        if (_tcslen(cmdInputBuffer) == 0) { // Enter vazio
            if (WaitForSingleObject(g->hConsoleOutputMutex, INFINITE) == WAIT_OBJECT_0) {
                _tprintf(_T("\r%*s\r"), 80, _T(""));
                _tprintf(prompt);
                fflush(stdout);
                ZeroMemory(g->adminInputLine, sizeof(g->adminInputLine));
                ReleaseMutex(g->hConsoleOutputMutex);
            }
            continue;
        }

        LOG_DEBUG(_T("threadAdminConsole: Comando digitado: '%s'"), cmdInputBuffer);

        TCHAR* tokenComando = _tcstok_s(cmdInputBuffer, _T(" "), &pContext);
        TCHAR* argumentoCmd = _tcstok_s(NULL, _T(" \t\n\r"), &pContext);

        if (tokenComando == NULL) {
            if (WaitForSingleObject(g->hConsoleOutputMutex, INFINITE) == WAIT_OBJECT_0) {
                _tprintf(_T("\r%*s\r"), 80, _T(""));
                _tprintf(prompt);
                fflush(stdout);
                ZeroMemory(g->adminInputLine, sizeof(g->adminInputLine));
                ReleaseMutex(g->hConsoleOutputMutex);
            }
            continue;
        }

        comando_admin cmdAdmin = checkComandoAdmin(tokenComando);
        TCHAR output_buffer[BUFFER_SIZE * 2];

        switch (cmdAdmin) {
        case listar: {
            TCHAR* listaJogStr = listarJogadores(g->listaJogadores);
            if (listaJogStr) {
                StringCchPrintf(output_buffer, _countof(output_buffer), _T("--- Lista de Jogadores (Admin) ---\n%s"), listaJogStr);
                escreverOutput(g, _T("%s"), output_buffer);
                free(listaJogStr);
            }
            else
                escreverOutput(g, _T("Erro ao obter lista de jogadores."));
            break;
        }
        case acelerar: {
            if (ritmo > TEMPO) {
                ritmo -= TEMPO;
                StringCchPrintf(output_buffer, _countof(output_buffer), _T("Ritmo ACELERADO. Novo intervalo: %.2f segundos."), (double)ritmo / TEMPO);
            }
            else
                StringCchPrintf(output_buffer, _countof(output_buffer), _T("Ritmo já está no mínimo (%.2f segundos). Não é possível acelerar mais."), (double)ritmo / TEMPO);

            escreverOutput(g, _T("%s"), output_buffer);
            break;
        }
        case travar: {
            ritmo += TEMPO;
            StringCchPrintf(output_buffer, _countof(output_buffer), _T("Ritmo TRAVADO. Novo intervalo: %.2f segundos."), (double)ritmo / TEMPO);
            escreverOutput(g, _T("%s"), output_buffer);
            break;
        }
        case excluir: {
            if (argumentoCmd == NULL || _tcslen(argumentoCmd) == 0) {
                escreverOutput(g, _T("Uso: /excluir <id_jogador>"));
                break;
            }
            int idJogador = _ttoi(argumentoCmd);
            if (idJogador <= 0) {
                escreverOutput(g, _T("ID inválido. Use um número positivo."));
                break;
            }
            if (WaitForSingleObject(g->listaJogadores->g_hMutexJogadores, INFINITE) != WAIT_OBJECT_0) {
                escreverOutput(g, _T("ERRO ao acessar mutex da lista de jogadores."));
                break;
            }
            if (removerJogador(g->listaJogadores, idJogador)) {
                StringCchPrintf(output_buffer, _countof(output_buffer), _T("Jogador com ID %d foi excluído com sucesso."), idJogador);
            }
            else {
                StringCchPrintf(output_buffer, _countof(output_buffer), _T("ERRO: Jogador com ID %d não encontrado."), idJogador);
            }
            ReleaseMutex(g->listaJogadores->g_hMutexJogadores);

            escreverOutput(g, _T("%s"), output_buffer);
            break;
        } // Fim do case excluir
        case iniciarbot: {
            if (argumentoCmd != NULL && _tcslen(argumentoCmd) > 0) {
                // argumentoCmd é o nome do bot (ex: "RoboEsperto")
                TCHAR nomeDoBot[NAME_SIZE];
                StringCchCopy(nomeDoBot, _countof(nomeDoBot), argumentoCmd);

                TCHAR linhaDeComandoBot[BUFFER_SIZE];
                StringCchPrintf(linhaDeComandoBot, _countof(linhaDeComandoBot), _T("%s %s %s"), NOME_EXECUTAVEL_BOT, PIPE_NAME, nomeDoBot);

                STARTUPINFO si;
                PROCESS_INFORMATION pi;

                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));

                escreverOutput(g, _T("Admin: Tentando iniciar bot '%s' com comando: %s"), nomeDoBot, linhaDeComandoBot);

                if (CreateProcess(NULL,         // Nome do módulo (usa linha de comando)
                    linhaDeComandoBot,          // Linha de comando
                    NULL,                       // Atributos de segurança do processo
                    NULL,                       // Atributos de segurança da thread
                    FALSE,                      // Herança de handles (FALSE=não herda)
                    0,                          // Flags de criação (ex: CREATE_NEW_CONSOLE para nova janela)
                    NULL,                       // Ambiente (usa o do pai)
                    NULL,                       // Diretório atual (usa o do pai)
                    &si,                        // Ponteiro para STARTUPINFO
                    &pi)                        // Ponteiro para PROCESS_INFORMATION
                    ) {
                    escreverOutput(g, _T("Admin: Bot '%s' iniciado com sucesso. PID: %lu, Thread ID: %lu"), nomeDoBot, pi.dwProcessId, pi.dwThreadId);

                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
                else
                    escreverOutput(g, _T("Admin: ERRO ao iniciar bot '%s'. Código de erro: %lu"), nomeDoBot, GetLastError());
            }
            else
                escreverOutput(g, _T("Uso: /iniciarbot <nome_do_bot>"));
            break;
        }
        case encerrar:
            escreverOutput(g, _T("Comando /encerrar recebido. Servidor a desligar..."));
            if (WaitForSingleObject(g->listaJogadores->g_hMutexJogadores, INFINITE) == WAIT_OBJECT_0) {
                run = false;
                for (int i = 0; i < g->listaJogadores->num_jogadores; i++) {
                    if (g->listaJogadores->jogadores[i].hPipeCliente != INVALID_HANDLE_VALUE) {
                        removerJogador(g->listaJogadores, g->listaJogadores->jogadores[i].id_jogador);
                        FlushFileBuffers(g->listaJogadores->jogadores[i].hPipeCliente);
                        DisconnectNamedPipe(g->listaJogadores->jogadores[i].hPipeCliente);
                        CloseHandle(g->listaJogadores->jogadores[i].hPipeCliente);
                    }
                }
                ReleaseMutex(g->listaJogadores->g_hMutexJogadores);
            }
            escreverOutput(g, _T("Todos os clientes foram notificados para desconectar."));
            break;
        case invalido_admin:
        default:
            StringCchPrintf(output_buffer, _countof(output_buffer), _T("Comando de administrador desconhecido: '%s'\nComandos: /listar, /excluir <id>, /encerrar, ..."), tokenComando);
            escreverOutput(g, _T("%s"), output_buffer);
            break;
        }

        if (run && cmdAdmin != encerrar) {
            if (WaitForSingleObject(g->hConsoleOutputMutex, INFINITE) == WAIT_OBJECT_0) {
                _tprintf(_T("\r%*s\r"), 80, _T(""));
                _tprintf(prompt);
                fflush(stdout);
                ZeroMemory(g->adminInputLine, sizeof(g->adminInputLine));
                ReleaseMutex(g->hConsoleOutputMutex);
            }
        }

    } // fim do while(run)

    LOG_DEBUG(_T("Thread Admin Console terminando."));
    return 0;
}

void offArbitro(globais* g) {
    if (g->mp->dados != NULL) UnmapViewOfFile(g->mp->dados);
    if (g->mp->mapFile != NULL) CloseHandle(g->mp->mapFile);
    if (g->mp->hMutex != NULL) CloseHandle(g->mp->hMutex);
    if (g->mp->hEvento != NULL) CloseHandle(g->mp->hEvento);
    if (g->hConsoleOutputMutex != NULL) CloseHandle(g->hConsoleOutputMutex);
    if (g->listaJogadores->g_hMutexJogadores != NULL) CloseHandle(g->listaJogadores->g_hMutexJogadores);
    g->mp->dados = NULL;
    g->mp->mapFile = NULL;
    g->mp->hMutex = NULL;
    g->mp->hEvento = NULL;
    g->listaJogadores->g_hMutexJogadores = NULL;
    g->mp = NULL;
    SetConsoleCtrlHandler(CtrlHandler, FALSE); // Desregistar
}

int setup(globais* g) {
    int erro = 0;
    LOG_DEBUG(_T("SETUP: Iniciando setup..."));

    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
        _tprintf(_T("ERRO: Não foi possível definir o handler da consola no árbitro.\n"));

    g->mp->hMutex = CreateMutex(NULL, FALSE, NOME_MUTEX);
    if (g->mp->hMutex == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Mutex (%lu).\n"), GetLastError());
        erro++;
    }

    g->mp->hEvento = CreateEvent(NULL, TRUE, FALSE, NOME_EVENTO);
    if (g->mp->hEvento == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Evento (%lu).\n"), GetLastError());
        erro++;
    }

    g->mp->mapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(MP),
        NOME_MP
    );

    if (g->mp->mapFile == NULL) {
        _tprintf(_T("ERRO: Falha ao criar FileMapping (%lu).\n"), GetLastError());
        erro++;
    }

    g->mp->dados = (MP*)MapViewOfFile(
        g->mp->mapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(MP)
    );
    if (g->mp->dados == NULL) {
        _tprintf(_T("ERRO: Falha ao mapear a memória (%lu).\n"), GetLastError());
        erro++;
    }

    g->listaJogadores->g_hMutexJogadores = CreateMutex(NULL, FALSE, NOME_MUTEX_JOGADORES);
    if (g->listaJogadores->g_hMutexJogadores == NULL) {
        _tprintf(_T("ERRO: Falha ao criar Mutex para jogadores (%lu).\n"), GetLastError());
        erro++;
    }

    if (!carregarDicionario()) {
        _tprintf(_T("ERRO: Falha ao carregar o dicionário.\n"));
        erro++;
    }

    g->listaJogadores->num_jogadores = 0;
    g->listaJogadores->prox_id_jogador = 1;

    WaitForSingleObject(g->mp->hMutex, INFINITE);
    ZeroMemory(g->mp->dados, sizeof(MP));
    ZeroMemory(g->listaJogadores->jogadores, sizeof(g->listaJogadores->jogadores));
    ReleaseMutex(g->mp->hMutex);

    return erro;
}

int _tmain(int argc, LPTSTR argv[]) {
#ifdef UNICODE
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    srand((unsigned int)time(NULL));
    memoria_partilhada mp = { 0 };
    lista_jogadores listaJogadores = { 0 };
    globais globals = { 0 };

    globals.mp = &mp;
    globals.listaJogadores = &listaJogadores;
    globals.hConsoleOutputMutex = CreateMutex(NULL, FALSE, _T("ConsoleOutputMutex"));
    ZeroMemory(globals.adminInputLine, sizeof(globals.adminInputLine));
    globals.adminInputCursorPos = 0;

    LOG_DEBUG(_T("Iniciando o árbitro..."));

    int erro = setup(&globals);
    if (erro != 0) {
        _tprintf(TEXT("Falha no setup. Encerrando.\n"));
        offArbitro(&globals);
        _tprintf(TEXT("\nPressione Enter para sair.\n"));
        _gettchar();
        return erro;
    }

    LOG_DEBUG(_T("Árbitro iniciado com sucesso. Esperando por conexões de clientes..."));

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    HANDLE hThreads[3];
    hThreads[0] = CreateThread(NULL, 0, threadGeradorLetras, &globals, 0, NULL);
    hThreads[1] = CreateThread(NULL, 0, threadGereCliente, &globals, 0, NULL);
    hThreads[2] = CreateThread(NULL, 0, threadAdminConsole, &globals, 0, NULL);

    if (hThreads[0] == NULL || hThreads[1] == NULL || hThreads[2] == NULL) {
        _tprintf(_T("ERRO: Falha ao criar threads (%lu).\n"), GetLastError());
        if (hThreads[0] != NULL) CloseHandle(hThreads[0]);
        if (hThreads[1] != NULL) CloseHandle(hThreads[1]);
        if (hThreads[2] != NULL) CloseHandle(hThreads[2]);
        offArbitro(&globals);
        return 1;
    }

    WaitForMultipleObjects(3, hThreads, TRUE, INFINITE);
    CloseHandle(hThreads[0]);
    CloseHandle(hThreads[1]);
    CloseHandle(hThreads[2]);

    offArbitro(&globals);
    return 0;
}