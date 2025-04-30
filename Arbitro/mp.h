#ifndef MEMORIA_PARTILHADA
#define MEMORIA_PARTILHADA

#include <windows.h>
#include <tchar.h>

#define MAXLETRAS 10

#define NOME_MP _T("MP")
#define NOME_MUTEX _T("MutexMP")
#define NOME_EVENTO _T("EventoMP")

typedef struct {
	TCHAR letras[MAXLETRAS];
} MP;

#endif 
#pragma once
