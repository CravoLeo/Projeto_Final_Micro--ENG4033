#include <SPI.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <JKSButton.h>
#include <TinyDatabase_Arduino.h>
#include <MFRC522.h>
#include <string.h>

/* ---------------------------------------------------*/
/* ------------ DEFINIÇÕES DE HARDWARE ---------------*/
/* ---------------------------------------------------*/

#define SS_PIN 53     // MFRC522
#define RST_PIN 32
MFRC522 mfrc522(SS_PIN, RST_PIN);

#define BUTTON_PIN 47 // Botão físico exportar PR
unsigned long lastButtonPress = 0;
const long debounceDelay = 500;
int ultimoEstadoBotao = HIGH;

int campainhaPassiva = 37;   // buzzer passivo (opcional)

/* --- CONTROLE DE MENSAGEM NA TELA (ACESSO OK / NEGADO) --- */
unsigned long tempoInicioMensagem = 0;
const long duracaoMensagem = 3000; // 3 segundos
bool exibindoMensagem = false;     // Indica se estamos no modo "Mensagem"

/* --- CONSTANTE DE TESTE, MANTIDA --- */
char DADOS_PRESENCA_TESTE[] = "PR_ALL:1A2B3C4D,16,40|5E6F7G8H,10,0|9I0J1K2L,8,55";

/* --- TFT + RTC --- */
MCUFRIEND_kbv tela;
RTC_PCF8563 rtc;

/* --- TOUCHSCREEN --- */
TouchScreen touch(6, A1, A2, 7, 300);
// Calibração (ajuste se precisar)
const int TS_LEFT = 145;
const int TS_RT   = 887;
const int TS_TOP  = 934;
const int TS_BOT  = 158;

/* ---------------------------------------------------*/
/* ---------------- ESTADO DE TELAS ------------------*/
/* ---------------------------------------------------*/

const int TELA_PADRAO   = 0;
const int TELA_TURMA    = 1;
const int TELA_CONFIG   = 2;
const int TELA_ESTAT    = 3;
const int TELA_INVALIDO = 4;

int telaAtual = TELA_PADRAO;

/* ---------------------------------------------------*/
/* --------------- CONTROLE DE TEMPO -----------------*/
/* ---------------------------------------------------*/

unsigned long ultimoUpdate = 0;

/* ---------------------------------------------------*/
/* -------------------- TINYDATABASE -----------------*/
/* ---------------------------------------------------*/

MemoryManager mem;

Column tabela_alunos[] = {
  { "Id", "CHAR10" },
  { "Nm", "CHAR30" }
};

char nome_tabela[] = "AL";

Column tabela_horarios[] = {
  { "Id", "CHAR10" },
  { "Dy", "CHAR4" },
  { "Hi", "BYTE" },
  { "Mi", "BYTE" },
  { "Hf", "BYTE" },
  { "Mf", "BYTE" }
};

char nome_tabela_hor[] = "HR";

Column tabela_presenca[] = {
  { "Id", "CHAR10" },
  { "Hi", "BYTE" },
  { "Mi", "BYTE" }
};

char nome_tabela_presenca[] = "PR";

/* ---------------------------------------------------*/
/* ------- ESTRUTURAS DE TURMA / ESTATÍSTICAS -------*/
/* ---------------------------------------------------*/

const int MAX_ALUNOS = 20;

struct Aluno {
  uint32_t tag;     // UID em forma numérica (para desenhar)
  uint16_t cont;    // número de presenças (contado na PR)
  bool     ativo;
  char     nome[16];
};

struct UltimoRegistro {
  uint32_t tag;     // UID numérico
  uint8_t  hora;
  uint8_t  minuto;
};

struct Estat {
  uint16_t       total;      // total de registros na PR
  UltimoRegistro ultimos[5];
};

Aluno turma[MAX_ALUNOS];
Estat estat;
uint16_t nAlunos     = 0;
uint32_t tagAtual    = 0;    // numérico (para usar na tela inválido, se usar)
String  tagAtualUID  = "";   // UID HEX (para salvar na AL depois, se desejar)

/* ---------------------------------------------------*/
/* -------------------- BOTÕES TOUCH -----------------*/
/* ---------------------------------------------------*/

JKSButton btTurma;
JKSButton btCad;
JKSButton btEst;
JKSButton btVoltar;
JKSButton btSalvar;
JKSButton btCancelar;
JKSButton btCadInvalido;
JKSButton btSync;
JKSButton btConfig;

/* ---------------------------------------------------*/
/* ------------------- FUNÇÕES SOM -------------------*/
/* ---------------------------------------------------*/

void somPermitido() {
  int frequencia = 440;
  int duracaoEmMs = 400;
  tone(campainhaPassiva, frequencia, duracaoEmMs);
}

void somNegado() {
  int frequencia = 220;
  int duracaoEmMs = 250;
  tone(campainhaPassiva, frequencia, duracaoEmMs);
}

/* ---------------------------------------------------*/
/* ------------Criação das funcoes DB-----------------*/
/* ---------------------------------------------------*/


void cria_tabela_presenca() {
  int status = mem.CREATE_TABLE(nome_tabela_presenca, 10, 3, tabela_presenca);
  if (status == STATUS_TABLE_CREATED)
    Serial.println("Tabela 'PR' criada com sucesso.");
  else if (status == STATUS_TABLE_EXIST)
    Serial.println("Tabela 'PR' já existe. (Se a estrutura antiga, use 'limparPr' ou 'reset').");
  else if (status == STATUS_INSUF_MEMORY)
    Serial.println("Falha: memória insuficiente para criar tabela PR.");
  else
    Serial.println("Falha ao criar tabela PR (status desconhecido).");
}

void cria_tabela_alunos() {
  int status = mem.CREATE_TABLE(nome_tabela, 20, 2, tabela_alunos);
  if (status == STATUS_TABLE_CREATED)
    Serial.println("Tabela 'AL' criada com sucesso.");
  else if (status == STATUS_TABLE_EXIST)
    Serial.println("Tabela 'AL' já existe.");
  else if (status == STATUS_INSUF_MEMORY)
    Serial.println("Falha: memória insuficiente para criar tabela AL.");
}

void insere_linha_alunos(String uid, String nome) {
  uid.toUpperCase();
  
  char uidChar[11];
  char nomeChar[31];
  uid.toCharArray(uidChar, sizeof(uidChar));
  nome.toCharArray(nomeChar, sizeof(nomeChar));

  int addrAL = mem.ON("AL");

  if (addrAL < 0){
    Serial.println("[AL ERROR] Tabela AL não encontrada");
    return;
  }
  TableData alunos(addrAL);
  alunos.INSERT("Id", uidChar)
    .INSERT("Nm", nomeChar)
    .DONE();

  Serial.print("[AL] Inserido: ");
  Serial.println(nome);
}

void insere_horario(String uid, String dia, uint8_t hi, uint8_t mi, uint8_t hf, uint8_t mf) {
  Serial.println("[HR DBG] >>> Entrou em insere_horario");

  char uidChar[11];
  char diaChar[5];
  uid.toUpperCase();
  dia.toUpperCase();

  uid.toCharArray(uidChar, sizeof(uidChar));
  dia.toCharArray(diaChar, sizeof(diaChar));

  Serial.print("[HR DBG] uidChar=");
  Serial.print(uidChar);
  Serial.print(" diaChar=");
  Serial.println(diaChar);

  Serial.println("[HR DBG] Antes de mem.TO(...)");
  int addrHR = mem.ON("HR");

  if (addrHR < 0){
    Serial.println("[HR ERROR] Tabela HR não encontrada");
    return;
  }

  TableData hr(addrHR);

  hr.INSERT("Id", uidChar)
    .INSERT("Dy", diaChar)
    .INSERT("Hi", &hi)
    .INSERT("Mi", &mi)
    .INSERT("Hf", &hf)
    .INSERT("Mf", &mf)
    .DONE();

  Serial.println("[HR DBG] Depois de mem.TO(...).DONE()");

  Serial.print("[HR] Horário inserido para ");
  Serial.println(uidChar);
}

void cria_tabela_horarios() {
  int status = mem.CREATE_TABLE(nome_tabela_hor, 40, 6, tabela_horarios);
  if (status == STATUS_TABLE_CREATED)
    Serial.println("Tabela 'HR' criada com sucesso.");
  else if (status == STATUS_TABLE_EXIST)
    Serial.println("Tabela 'HR' já existe.");
  else if (status == STATUS_INSUF_MEMORY)
    Serial.println("Falha: memória insuficiente para criar tabela HR.");
}

void limpar_tabela_alunos() {
  int apagados = 0;

  int addrAL = mem.ON("AL");
  if (addrAL < 0) {
    Serial.println("[AL] Erro: tabela nao encontrada.");
    return;
  }

  TableData alunos(addrAL);

  alunos.DELETE_ALL(apagados).DONE();

  Serial.print("Linhas apagadas da tabela 'AL': ");
  Serial.println(apagados);
}

void limpar_tabela_horarios() {
  int apagados = 0;

  int addrHR = mem.ON("HR");
  if (addrHR < 0) {
    Serial.println("[HR] Erro: tabela nao encontrada.");
    return;
  }

  TableData hr(addrHR);

  hr.DELETE_ALL(apagados).DONE();

  Serial.print("Linhas apagadas da tabela 'HR': ");
  Serial.println(apagados);
}

void limpar_tabela_presenca(){
  int apagados = 0;

  int addrPR = mem.ON("PR");
  if (addrPR < 0) {
    Serial.println("[PR] Erro: tabela nao encontrada.");
    return;
  }

  TableData presenca(addrPR);

  presenca.DELETE_ALL(apagados).DONE();

  Serial.print("Linhas apagadas da tabela 'Pr': ");
  Serial.println(apagados);
}

void listar_tabela_horarios() {

  int addrHR = mem.ON("HR");
  if (addrHR < 0){
    Serial.println("[HR] Erro: tabela não encontrada");
    return;
  }

  TableData hr(addrHR);

  uint8_t total = hr.COUNT();

  Serial.print("Total de horários: ");
  Serial.println(total);

  if (total == 0) {
    Serial.println("Nenhum horário cadastrado.");
    return;
  }

  Serial.println("----- TABELA HR -----");

  char    id[11];
  char    dia[4];
  uint8_t hi, mi, hf, mf;

  for (uint8_t i = 0; i < total; i++) {
    hr.SELECT("Id", id, i)
      .SELECT("Dy", dia, i)
      .SELECT("Hi", &hi, i)
      .SELECT("Mi", &mi, i)
      .SELECT("Hf", &hf, i)
      .SELECT("Mf", &mf, i)
      .DONE();

    Serial.print("[");
    Serial.print(i);
    Serial.print("] RFID=");
    Serial.print(id);
    Serial.print("  Dia=");
    Serial.print(dia);
    Serial.print("  Horário=");
    if (hi < 10) Serial.print("0");
    Serial.print(hi);
    Serial.print(":");
    if (mi < 10) Serial.print("0");
    Serial.print(mi);
    Serial.print(" - ");
    if (hf < 10) Serial.print("0");
    Serial.print(hf);
    Serial.print(":");
    if (mf < 10) Serial.print("0");
    Serial.println(mf);
  }
}

void listar_tabela_presenca() {

  int addrHR = mem.ON("PR");

  if (addrHR < 0){
    Serial.println("[HR] Erro: tabela não encontrada");
    return;
  }

  TableData hr(addrHR);

  uint8_t total = hr.COUNT();

  Serial.print("Total de horários: ");
  Serial.println(total);

  if (total == 0) {
    Serial.println("Nenhum horário cadastrado.");
    return;
  }

  Serial.println("----- TABELA PR -----");

  char    id[11];
  char    dia[4];
  uint8_t hi, mi, hf, mf;

  for (uint8_t i = 0; i < total; i++) {
    hr.SELECT("Id", id, i)
      .SELECT("Hi", &hi, i)
      .SELECT("Mi", &mi, i)
      .DONE();

    Serial.print("[");
    Serial.print(i);
    Serial.print("] RFID=");
    Serial.print(id);
    Serial.print("  Horário=");
    if (hi < 10) Serial.print("0");
    Serial.print(hi);
    Serial.print(":");
    if (mi < 10) Serial.print("0");
    Serial.print(mi);
  }
}

void listar_tabela_alunos() {

  int addrAL = mem.ON("AL");

  if (addrAL < 0){
    Serial.println("[AL] Erro: tabela não encontrada");
    return;
  }

  TableData alunos(addrAL);

  uint8_t total = alunos.COUNT();

  Serial.print("Total de registros: ");
  Serial.println(total);

  if (total == 0) {
    Serial.println("Nenhum registro encontrado.");
    return;
  }

  Serial.println("----- TABELA AL -----");

  char    id[11];
  char    nome[31];

  for (uint8_t i = 0; i < total; i++) {
    alunos.SELECT("Id", id, i)
      .SELECT("Nm", nome, i)
      .DONE();
    
    Serial.print("[");
    Serial.print(i);
    Serial.print("] Id=");
    Serial.print(id);
    Serial.print("  Nome=");
    Serial.println(nome);
  }
}



/* ---------------------------------------------------*/
/* ------- FUNÇÕES DE PRESENÇA / HORÁRIO -------------*/
/* ---------------------------------------------------*/

// Função Adaptada (assumindo que o horário já foi obtido do RTC e validado)
void registra_presenca_agora(char *uidChar, uint8_t hAtual, uint8_t mAtual) {
  // 1. Verificar se o aluno existe na tabela AL
  int addrAL = mem.ON("AL");

  if (addrAL < 0) {
  Serial.println("[AL] Erro: tabela AL nao encontrada em registra_presenca_agora.");
  return;
  }

  TableData alunos(addrAL);

  uint8_t totalAl = alunos.COUNT();
  bool existeNaAL = false;
  char idAL[11];

  for (uint8_t i = 0; i < totalAl; i++) {
    alunos.SELECT("Id", idAL, i).DONE();
    if (strcmp(idAL, uidChar) == 0) {
      existeNaAL = true;
      break;
    }
  }

  if (!existeNaAL) {
    Serial.print("[PR] Falha: RFID '");
    Serial.print(uidChar);
    Serial.println("' não encontrado na tabela de Alunos (AL).");
    return;
  }

  // 2. Inserir na PR
  int addrPr = mem.ON("PR");

  if (addrPr < 0) {
  Serial.println("[PR] Tabela PR não encontrada.");
  return;
  }
  TableData presenca(addrPr);
  
  presenca
    .INSERT("Id", (char*)uidChar)
    .INSERT("Hi", &hAtual)
    .INSERT("Mi", &mAtual)
    .DONE();

  Serial.println("----------------------------");
  Serial.print(" PRESENÇA REGISTRADA para ");
  Serial.print(uidChar);
  Serial.print(" às ");
  if (hAtual < 10) Serial.print("0");
  Serial.print(hAtual);
  Serial.print(":");
  if (mAtual < 10) Serial.print("0");
  Serial.println(mAtual);
  Serial.println("----------------------------");
}

void registra_presenca_agora(const String &uid, uint8_t hAtual, uint8_t mAtual) { //wrapper
  char uidChar[11];
  String upper = uid;
  upper.toUpperCase();
  upper.toCharArray(uidChar, sizeof(uidChar));
  registra_presenca_agora(uidChar, hAtual, mAtual);
}

char* dia_para_cstr(uint8_t dayIndex) {
  switch (dayIndex) {
    case 1: return "SEG";
    case 2: return "TER";
    case 3: return "QUA";
    case 4: return "QUI";
    case 5: return "SEX";
    case 6: return "SAB";
    case 0: return "DOM";
    default: return "???";
  }
}

bool esta_no_horario_correto(char *uidChar) {
  DateTime agora = rtc.now();

  const char *diaAtualChar = dia_para_cstr(agora.dayOfTheWeek());
  uint16_t minutosAgora = agora.hour() * 60 + agora.minute();
  int addrHR = mem.ON(nome_tabela_hor);

  Serial.print("\n[HR CHECK] Verificando acesso para ");
  Serial.print(uidChar);
  Serial.print(" em ");
  Serial.println(diaAtualChar);

  if (addrHR < 0) {
    Serial.println("[HR] Tabela HR não encontrada.");
    return false;
  }

  TableData hr(addrHR);
  uint8_t totalHr = hr.COUNT();

  for (uint8_t i = 0; i < totalHr; i++) {
    char idHR[11];
    char dyHR[5];

    hr.SELECT("Id", idHR, i)
      .SELECT("Dy", dyHR, i)
      .DONE();

    if (strcmp(idHR, uidChar) == 0 && strcmp(dyHR, diaAtualChar) == 0) {
      uint8_t Hi, Mi, Hf, Mf;

      hr.SELECT("Hi", &Hi, i)
        .SELECT("Mi", &Mi, i)
        .SELECT("Hf", &Hf, i)
        .SELECT("Mf", &Mf, i)
        .DONE();

      uint16_t minutosInicio = Hi * 60 + Mi;
      uint16_t minutosFim    = Hf * 60 + Mf;

      Serial.print("[HR] Horário agendado: ");
      Serial.print(Hi); Serial.print(":"); Serial.print(Mi);
      Serial.print(" a ");
      Serial.print(Hf); Serial.print(":"); Serial.println(Mf);

      if (minutosAgora >= minutosInicio && minutosAgora <= minutosFim) {
        Serial.println("[HR] ALUNO DENTRO DO HORÁRIO PERMITIDO.");
        return true;
      } else {
        Serial.println("[HR] ALUNO FORA DO HORÁRIO AGENDADO.");
      }
    }
  }

  Serial.println("[HR] Nenhum horário agendado encontrado ou compatível.");
  return false;
}


bool esta_no_horario_correto(const String &uid) { //wrapper 
    char uidChar[11];
    String upper = uid;
    upper.toUpperCase();
    upper.toCharArray(uidChar, sizeof(uidChar));
    return esta_no_horario_correto(uidChar);
}


bool aluno_ja_existe(String uid) {
  uid.toUpperCase();
  char uidChar[11];
  uid.toCharArray(uidChar, sizeof(uidChar));

  int addrAL = mem.ON("AL");

  if (addrAL < 0) {
    Serial.println("[AL] Erro: tabela AL nao encontrada em registra_presenca_agora.");
    return;
  }

  TableData alunos(addrAL);

  uint8_t totalAl = alunos.COUNT();
  char id[11];

  for (uint8_t i = 0; i < totalAl; i++) {
    alunos.SELECT("Id", id, i).DONE();
    if (strcmp(id, uidChar) == 0) {
      Serial.print("[AL] Ja existe aluno com Id="); Serial.println(id);
      return true;
    }
  }
  return false;
}

bool horario_ja_existe(String uid, String dia, int hi, int mi, int hf, int mf) {
  uid.toUpperCase();
  dia.toUpperCase();

  char uidChar[11];
  char diaChar[5];
  uid.toCharArray(uidChar, sizeof(uidChar));
  dia.toCharArray(diaChar, sizeof(diaChar));

  int addrHR = mem.ON("HR");

  if (addrHR < 0) {
    Serial.println("[HR] Erro: tabela AL nao encontrada em registra_presenca_agora.");
    return;
  }

  TableData hr(addrHR);

  uint8_t totalHr = hr.COUNT();

  char  id[11];
  char  dy[5];
  uint8_t Hi, Mi, Hf, Mf;

  for (uint8_t i = 0; i < totalHr; i++) {
    hr.SELECT("Id", id, i)
      .SELECT("Dy", dy, i)
      .SELECT("Hi", &Hi, i)
      .SELECT("Mi", &Mi, i)
      .SELECT("Hf", &Hf, i)
      .SELECT("Mf", &Mf, i)
      .DONE();

    if (strcmp(id, uidChar) == 0 &&
        strcmp(dy, diaChar) == 0 &&
        Hi == hi && Mi == mi &&
        Hf == hf && Mf == mf) {
      //Serial.println("[HR] Horario ja existe, nao vou duplicar.");
      return true;
    }
  }
  return false;
}

/* ---------------------------------------------------*/
/* ------ FUNÇÃO EXPORTAÇÃO PRESENÇA (PR_ALL) --------*/
/* ---------------------------------------------------*/

void exportar_tabela_presenca_single_line() {

  int addrPR = mem.ON("PR");

  if (addrPR < 0 ){
    Serial.println("[PR] Erro: tabela PR nao encontrada.");
  }

  TableData presenca(addrPR);

  uint8_t total = presenca.COUNT();
  String dados_completos = "PR_ALL:";

  if (total == 0) {
    Serial.println("PR_ALL:EMPTY");
    return;
  }

  char id[11];
  uint8_t hi, mi;

  // --- Vetor para armazenar RFIDs já vistos ---
  const uint8_t MAX_PRES = 50;   // limite (ajuste se quiser)
  String rfids_vistos[MAX_PRES];
  uint8_t vistos_count = 0;

  for (uint8_t i = 0; i < total; i++) {

    // Lê os dados brutos da tabela
    presenca.SELECT("Id", id, i)
      .SELECT("Hi", &hi, i)
      .SELECT("Mi", &mi, i)
      .DONE();

    String id_str = String(id);

    // --- 1. Verifica se este RFID já foi visto ---
    bool ja_existe = false;
    for (uint8_t j = 0; j < vistos_count; j++) {
      if (rfids_vistos[j] == id_str) {
        ja_existe = true;
        break;
      }
    }

    if (ja_existe) {
      continue;  // pula e não adiciona novamente
    }

    // --- 2. Armazena como "visto" ---
    rfids_vistos[vistos_count] = id_str;
    vistos_count++;

    // --- 3. Adiciona ao string final ---
    dados_completos += id_str;
    dados_completos += ",";

    if (hi < 10) dados_completos += "0";
    dados_completos += hi;
    dados_completos += ",";

    if (mi < 10) dados_completos += "0";
    dados_completos += mi;

    if (i < total - 1) {
      dados_completos += "|";
    }
  }

  Serial.println(dados_completos);
}

/* ---------------------------------------------------*/
/* ----------------- COISAS DE TELA ------------------*/
/* ---------------------------------------------------*/

/* Helpers para turma/estat */

uint32_t hexToUint32(char *s) {
  uint32_t val = 0;
  while (*s) {
    char c = *s++;
    int v;
    if      (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else continue;
    val = (val << 4) | v;
  }
  return val;
}

// Conta quantas presenças esse UID tem na tabela PR
uint16_t contarPresencasId(char *idBusca) {

  int addrpr = mem.ON("PR");
  if (addrpr < 0) {
    Serial.println("[PR] Tabela PR não encontrada em contarPresencasId.");
    return 0;
  }

  TableData presenca(addrpr);
  uint8_t total = presenca.COUNT();
  char id[11];
  uint16_t cont = 0;

  for (uint8_t i = 0; i < total; i++) {
    presenca.SELECT("Id", id, i).DONE();
    if (strcmp(id, idBusca) == 0) cont++;
  }
  return cont;
}

int buscarAluno(uint32_t tag) {
  for (int i = 0; i < nAlunos; i++) {
    if (turma[i].ativo && turma[i].tag == tag) return i;
  }
  return -1;
}

String nomeOuTag(uint32_t tag) {
  int idx = buscarAluno(tag);
  if (idx >= 0) {
    return String(turma[idx].nome);
  }
  char buf[9];
  sprintf(buf, "%08lX", (unsigned long)tag);
  return String(buf);
}

String primeiroNome(String nomeCompleto) {
  int espaco = nomeCompleto.indexOf(' ');
  if (espaco <= 0) return nomeCompleto;
  return nomeCompleto.substring(0, espaco);
}

String rfidCurto(uint32_t tag) {
  char buf[9];
  sprintf(buf, "%08lX", (unsigned long)tag); 
  String hexStr = String(buf);
  return hexStr.substring(0, 4);             
}

// Nome direto da tabela AL a partir do UID HEX
bool nomePorUID(char *uidChar, char *nomeOut, size_t tam) {
  int addrAl = mem.ON("AL");
  if (addrAl < 0) {
    Serial.println("[PR] Tabela PR não encontrada em contarPresencasId.");
    return 0;
  }
  TableData alunos(addrAl);
  uint8_t totalAl = alunos.COUNT();
  char id[11];
  char nome[31];

  for (uint8_t i = 0; i < totalAl; i++) {
    alunos.SELECT("Id", id, i)
          .SELECT("Nm", nome, i)
          .DONE();

    if (strcmp(id, uidChar) == 0) {
      strncpy(nomeOut, nome, tam - 1);
      nomeOut[tam - 1] = '\0';
      return true;
    }
  }
  return false;
}

String nomePorUID(const String &uid) { //wrapper
  char uidChar[11];
  uid.toCharArray(uidChar, sizeof(uidChar));

  char nome[31];
  if (nomePorUID(uidChar, nome, sizeof(nome))) {
    return String(nome);
  }
  return uid; // fallback: devolve o próprio UID
}

/* --- Sincronização das estruturas com o DB (só leitura) --- */

void atualizarTurmaDeDB() {
  int addrAL = mem.ON(nome_tabela); // "AL"
  if (addrAL < 0) { 
    Serial.println("[AL]] Tabela AL não encontrada.");
    return;
  }

  TableData alunos(addrAL);
 
  uint8_t totalAl = alunos.COUNT();
  nAlunos = (totalAl > MAX_ALUNOS) ? MAX_ALUNOS : totalAl;

  char id[11];
  char nome[31];

  for (uint16_t i = 0; i < nAlunos; i++) {
    alunos.SELECT("Id", id, i)
      .SELECT("Nm", nome, i)
      .DONE();


    turma[i].tag   = hexToUint32(id);
    turma[i].ativo = true;

    strncpy(turma[i].nome, nome, sizeof(turma[i].nome) - 1);
    
    turma[i].nome[sizeof(turma[i].nome) - 1] = '\0';

    turma[i].cont = contarPresencasId(id);
  }
}

void atualizarEstatDeDB() {
  int addrpr = mem.ON("PR");
  
  if (addrpr < 0) {
    Serial.println("[PR] Tabela PR não encontrada em contarPresencasId.");
    return 0;
  }

  TableData presenca(addrpr);

  uint8_t totalPR = presenca.COUNT();
  estat.total = totalPR;

  Serial.println(totalPR);
 
  for (int i = 0; i < 5; i++) {
    estat.ultimos[i].tag    = 0;
    estat.ultimos[i].hora   = 0;
    estat.ultimos[i].minuto = 0;
  }

  uint8_t mi;
  char id[11];
  uint8_t hi;
  

  for (uint8_t k = 0; k < totalPR; k++) {
    if (totalPR == 0) break;
    uint8_t idx = totalPR - 1 - k;
    idx = k;
    if (idx < 0) break;
    
    presenca.SELECT("Id", id, idx)
    .SELECT("Hi", &hi, idx)
    .SELECT("Mi", &mi, idx)
    .DONE();

    estat.ultimos[k].tag    = hexToUint32(id);
    estat.ultimos[k].hora   = hi;
    estat.ultimos[k].minuto = mi;
  }
}

/* --- Hora/Data --- */

void desenharHoraData() {
  DateTime agora = rtc.now();

  char hora[9];
  sprintf(hora, "%02d:%02d", agora.hour(), agora.minute());

  char data[11];
  sprintf(data, "%02d/%02d/%04d", agora.day(), agora.month(), agora.year());

  tela.fillRect(60, 270, 140, 20, TFT_BLACK);
  tela.setTextSize(2);
  tela.setTextColor(TFT_CYAN, TFT_BLACK);
  tela.setCursor(92, 270);
  tela.print(hora);
  tela.setTextColor(TFT_WHITE, TFT_BLACK);
  tela.setCursor(64, 290);
  tela.print(data);
}

void printCentralizado(String texto, int y, int tamanho,
                        uint16_t corTexto, uint16_t corFundo) {
  int16_t x1, y1;
  uint16_t w, h;
  tela.setTextSize(tamanho);
  tela.setTextColor(corTexto, corFundo);
  tela.getTextBounds(texto, 0, 0, &x1, &y1, &w, &h);
  int x = (tela.width() - w) / 2;
  tela.setCursor(x, y);
  tela.print(texto);
}


void printCentralizadoC(char *texto, int y, int tamanho,
                        uint16_t corTexto, uint16_t corFundo) {
  int16_t x1, y1;
  uint16_t w, h;
  tela.setTextSize(tamanho);
  tela.setTextColor(corTexto, corFundo);
  tela.getTextBounds(texto, 0, 0, &x1, &y1, &w, &h);
  int x = (tela.width() - w) / 2;
  tela.setCursor(x, y);
  tela.print(texto);
}


/* ---------------------------------------------------*/
/* ----------------- TELAS / BOTÕES ------------------*/
/* ---------------------------------------------------*/

void onTurma(JKSButton &b);
void onEstat(JKSButton &b);
void onVoltarPadrao(JKSButton &b);
void onSync(JKSButton &b); 
void onConfig(JKSButton &b);

void desenharTelaPadrao() {
  telaAtual = TELA_PADRAO;
  tela.fillScreen(TFT_BLACK);

  printCentralizado("Ola!", 40, 2, TFT_WHITE, TFT_BLACK);
  printCentralizado("Passe o cartao", 70, 2, TFT_WHITE, TFT_BLACK);
  printCentralizado("na regiao ao lado", 100, 2, TFT_WHITE, TFT_BLACK);

  // Agora só um botão de Configuracoes, no lugar do antigo "Sync"
  btConfig.init(&tela, &touch, 120, 235, 120, 40,
                TFT_WHITE, TFT_CYAN, TFT_BLACK,
                "Config", 2);
  btConfig.setPressHandler(onConfig);
}

void desenharTelaConfig() {
  telaAtual = TELA_CONFIG;
  tela.fillScreen(TFT_BLACK);

  printCentralizado("Configuracoes", 10, 2, TFT_WHITE, TFT_BLACK);

  // Botões de Turma, Infos e Sync nesta tela
  btTurma.init(&tela, &touch, 60, 120, 110, 40,
               TFT_WHITE, TFT_YELLOW, TFT_BLACK,
               "Turma", 2);

  btEst.init(&tela, &touch, 180, 120, 110, 40,
             TFT_WHITE, TFT_GREEN, TFT_BLACK,
             "Infos", 2);

  btSync.init(&tela, &touch, 120, 180, 120, 40,
              TFT_WHITE, TFT_CYAN, TFT_BLACK,
              "Sync", 2);

  btTurma.setPressHandler(onTurma);
  btEst.setPressHandler(onEstat);
  btSync.setPressHandler(onSync);

  // Botão Voltar (para voltar à tela padrão)
  btVoltar.init(&tela, &touch, 120, 295, 160, 40,
                TFT_WHITE, TFT_RED, TFT_BLACK,
                "Voltar", 2);
  btVoltar.setPressHandler(onVoltarPadrao);
}


// Versão NÃO BLOQUEANTE de mostrarAcesso
void mostrarAcesso(String tipo, String nome) {
  tela.fillScreen(TFT_BLACK);

  if (tipo.equalsIgnoreCase("nok")) {
    printCentralizado("ACESSO NEGADO", 80, 3, TFT_RED, TFT_BLACK);
  } else if (tipo.equalsIgnoreCase("ok")) {
    printCentralizado("ACESSO OK", 80, 3, TFT_GREEN, TFT_BLACK);
  }

  printCentralizado("RFID lido:", 150, 2, TFT_WHITE, TFT_BLACK);
  printCentralizado(nome,       175, 2, TFT_YELLOW, TFT_BLACK);

  exibindoMensagem   = true;
  tempoInicioMensagem = millis();
}

String tagCurta(uint32_t tag) {
  char buf[9];
  sprintf(buf, "%08lX", (unsigned long)tag);  
  return String(buf).substring(0, 4);
}

String nomeCompacto(const char *nome) {
  String s = String(nome);
  
  int esp = s.indexOf(' ');
  if (esp == -1) return s;   // não tem sobrenome → retorna tudo

  String primeiro = s.substring(0, esp);
  char inicial = s.charAt(esp + 1);

  String final = primeiro + " " + inicial + ".";
  return final;
}

void desenharTelaTurma() {
  telaAtual = TELA_TURMA;
  tela.fillScreen(TFT_BLACK);

  printCentralizado("Turma", 10, 2, TFT_WHITE, TFT_BLACK);

  tela.setTextSize(2);
  tela.setTextColor(TFT_WHITE, TFT_BLACK);
  tela.setCursor(10, 35);  tela.print("ID:");
  tela.setCursor(85, 35);  tela.print("Nome:");

  int y = 65;
  for (int i = 0; i < nAlunos; i++) {
    if (!turma[i].ativo) continue;

    String tag4 = tagCurta(turma[i].tag);
    tela.setCursor(10, y);
    tela.print(tag4);

    String nome1 = nomeCompacto(turma[i].nome);
    tela.setCursor(70, y);
    tela.print(nome1);

    y += 18;
    if (y > 260) break;
  }

  btVoltar.init(&tela, &touch, 120, 295, 160, 40,
                TFT_WHITE, TFT_RED, TFT_BLACK,
                "Voltar", 2);
  btVoltar.setPressHandler(onConfig);
}

void mostrarAcessoC(const char *tipo, const char *nome) {
  tela.fillScreen(TFT_BLACK);

  if (strcmp(tipo, "nok") == 0 || strcmp(tipo, "NOK") == 0) {
    printCentralizadoC("ACESSO NEGADO", 80, 3, TFT_RED, TFT_BLACK);
  } else {
    printCentralizadoC("ACESSO OK",     80, 3, TFT_GREEN, TFT_BLACK);
  }

  printCentralizadoC("RFID lido:", 150, 2, TFT_WHITE,  TFT_BLACK);
  printCentralizadoC(nome,         175, 2, TFT_YELLOW, TFT_BLACK);

  exibindoMensagem    = true;
  tempoInicioMensagem = millis();
}


void mostrarAcessoOk(const char *nome) {
  mostrarAcessoC("ok", nome);
}

void mostrarAcessoNok(const char *uid) {
  mostrarAcessoC("nok", uid);
}

void onTurma(JKSButton &b) {
  Serial.println("DEBUG: onTurma acionado");
  atualizarTurmaDeDB();
  desenharTelaTurma();
}

void onEstat(JKSButton &b) {
  Serial.println("DEBUG: onEstat acionado");
  atualizarEstatDeDB();
  desenharTelaEstat();
}

void onVoltarPadrao(JKSButton &b) {
  Serial.println("DEBUG: onVoltarPadrao acionado");
  desenharTelaPadrao();
  desenharHoraData();
}

void onSync(JKSButton &b) {
  tela.fillScreen(TFT_BLACK);
  printCentralizado("Sincronizando...", 110, 2, TFT_WHITE, TFT_BLACK);

  exportar_tabela_presenca_single_line();


  delay(700);

  limpar_tabela_presenca();


  tela.fillScreen(TFT_BLACK);
  printCentralizado("Sincronizado!", 110, 2, TFT_GREEN, TFT_BLACK);

  delay(700);

  // Volta para a tela principal
  desenharTelaPadrao();
  desenharHoraData();
}

void onConfig(JKSButton &b) {
  Serial.println("DEBUG: onConfig acionado");
  desenharTelaConfig(); 
}

String formataHoraMinuto(uint8_t h, uint8_t m) {
  char buf[6];
  sprintf(buf, "%02d:%02d", h, m);
  return String(buf);
}

void desenharTelaEstat() {
  telaAtual = TELA_ESTAT;
  tela.fillScreen(TFT_BLACK);

  printCentralizado("Estatisticas", 10, 2, TFT_WHITE, TFT_BLACK);

  String linhaTotal = "Total: ";
  linhaTotal += String(estat.total);
  printCentralizado(linhaTotal, 50, 2, TFT_WHITE, TFT_BLACK);

  printCentralizado("Ultimos 5 alunos:", 80, 2, TFT_WHITE, TFT_BLACK);

  int y = 115;

  tela.setTextSize(2);
  tela.setTextColor(TFT_WHITE, TFT_BLACK);

  for (int i = 0; i < 5; i++) {
    if (estat.ultimos[i].tag == 0) continue;

    uint32_t tag  = estat.ultimos[i].tag;


    // Nome completo a partir do UID (AL ou tag em hex)
    String nomeCompleto = nomeOuTag(tag);

    // Nome formatado: primeiro nome + inicial do sobrenome
    String nomeFmt = nomeCompacto(nomeCompleto.c_str());

    // Horário HH:MM
    String hora = formataHoraMinuto(estat.ultimos[i].hora,
                                    estat.ultimos[i].minuto);

    // Colunas: [UID4] [Nome compactado] [Hora]

    tela.setCursor(10, y);
    tela.print(nomeFmt);

    tela.setCursor(160, y);
    tela.print(hora);

    y += 20;
    if (y > 260) break;
  }

  btVoltar.init(&tela, &touch, 120, 295, 160, 40,
                TFT_WHITE, TFT_RED, TFT_BLACK,
                "Voltar", 2);
  btVoltar.setPressHandler(onConfig);
}

void desenharTelaInvalido() {
  telaAtual = TELA_INVALIDO;
  tela.fillScreen(TFT_BLACK);

  printCentralizado("RFID INVALIDO", 40, 2, TFT_RED, TFT_BLACK);

  String linhaTag = "Tag: ";
  linhaTag += String(tagAtual);
  printCentralizado(linhaTag, 80, 2, TFT_WHITE, TFT_BLACK);

  printCentralizado("Cadastrar na turma?", 120, 2, TFT_WHITE, TFT_BLACK);

  btCadInvalido.init(&tela, &touch, 60, 295, 100, 40,
                     TFT_WHITE, TFT_GREEN, TFT_BLACK,
                     "Sim", 2);
  btVoltar.init(&tela, &touch, 180, 295, 100, 40,
                TFT_WHITE, TFT_RED, TFT_BLACK,
                "Nao", 2);

  btVoltar.setPressHandler(onVoltarPadrao);
}

void processarBotoes() {
  if (telaAtual == TELA_PADRAO) {
    btConfig.process();                   // <<--- agora só o botão Config
  } else if (telaAtual == TELA_TURMA) {
    btVoltar.process();
  } else if (telaAtual == TELA_ESTAT) {
    btVoltar.process();
  } else if (telaAtual == TELA_CONFIG) {  // <<--- NOVO
    btTurma.process();
    btEst.process();
    btSync.process();
    btVoltar.process();
  } else if (telaAtual == TELA_INVALIDO) {
    btCadInvalido.process();
    btVoltar.process();
  }
}


/* ---------------------------------------------------*/
/* ---------------- LEITURA RFID ---------------------*/
/* ---------------------------------------------------*/

void uidToString(const MFRC522::Uid *uid, char *out, size_t outSize) {
  uint8_t n = uid->size;
  if (outSize < n * 2 + 1) return;

  for (uint8_t i = 0; i < n; i++) {
    sprintf(&out[i * 2], "%02X", uid->uidByte[i]);
  }

  out[n * 2] = '\0';
}

void checaCartaoRFID() {
  int fm0 = freeMemory();
  Serial.print("FM0 (entrada checaCartaoRFID): ");
  Serial.println(fm0);

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial())   return;

  int fm1 = freeMemory();
  Serial.print("FM1 (apos ler UID): ");
  Serial.println(fm1);

  char uidChar[11];
  uidToString(&mfrc522.uid, uidChar, sizeof(uidChar));
  for (int i = 0; uidChar[i]; ++i) {
    uidChar[i] = toupper(uidChar[i]);
  }

  Serial.print("Cartao lido: ");
  Serial.println(uidChar);

  DateTime agora = rtc.now();
  uint8_t h = agora.hour();
  uint8_t m = agora.minute();

  int fm2 = freeMemory();
  Serial.print("FM2 (apos DateTime): ");
  Serial.println(fm2);

  bool dentro = esta_no_horario_correto(uidChar);

  int fm3 = freeMemory();
  Serial.print("FM3 (apos esta_no_horario_correto): ");
  Serial.println(fm3);

  if (dentro) {
    registra_presenca_agora(uidChar, h, m);

    int fm4 = freeMemory();
    Serial.print("FM4 (apos registra_presenca_agora): ");
    Serial.println(fm4);

    atualizarTurmaDeDB();

    int fm5 = freeMemory();
    Serial.print("FM5 (apos atualizarTurmaDeDB): ");
    Serial.println(fm5);

    atualizarEstatDeDB();

    int fm6 = freeMemory();
    Serial.print("FM6 (apos atualizarEstatDeDB): ");
    Serial.println(fm6);

    char nome[31];
    if (!nomePorUID(uidChar, nome, sizeof(nome))) {
      strncpy(nome, uidChar, sizeof(nome));
      nome[sizeof(nome)-1] = '\0';
    }

    int fm7 = freeMemory();
    Serial.print("FM7 (apos nomePorUID): ");
    Serial.println(fm7);

    somPermitido();
    mostrarAcessoOk(nome);

    int fm8 = freeMemory();
    Serial.print("FM8 (apos mostrarAcessoOk): ");
    Serial.println(fm8);

  } else {
    Serial.println("ACESSO NEGADO: Fora do horario agendado.");
    somNegado();
    mostrarAcessoNok(uidChar);

    int fmNok = freeMemory();
    Serial.print("FMnok (apos mostrarAcessoNok): ");
    Serial.println(fmNok);
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}


/* ---------------------------------------------------*/
/* ------- SERIAL: Dado: e comandos do Python --------*/
/* ---------------------------------------------------*/

int contarVirgulas(const String &s) {
  int c = 0;
  for (int i = 0; i < s.length(); i++) {
    if (s[i] == ',') c++;
  }
  return c;
}

bool processa_dado_serial(String entrada) {
  // debug: mostra o que chegou
  Serial.print("[DBG] Linha bruta recebida: '");
  Serial.print(entrada);
  Serial.println("'");

  // remove prefixo Dado: ou dado:
  if (entrada.startsWith("Dado:") || entrada.startsWith("dado:")) {
    entrada.remove(0, 5);
  }
  entrada.trim();

  // --- separa em 5 partes usando vírgula ---
  String partes[5];
  int idx = 0;

  while (idx < 5) {
    int pos = entrada.indexOf(',');
    if (pos == -1) {
      // se for a última parte, pega o resto
      partes[idx] = entrada;
      break;
    }
    partes[idx] = entrada.substring(0, pos);
    entrada = entrada.substring(pos + 1);
    entrada.trim();
    idx++;
  }

  int total = idx + 1;  // número de partes preenchidas

  if (total != 5) {
    Serial.print("[ERRO] Formato invalido. Partes encontradas = ");
    Serial.println(total);
    return false;
  }

  String rfid = partes[0];
  String nome = partes[1];
  String dia  = partes[2];
  String hEnt = partes[3];
  String hSai = partes[4];

  rfid.trim();
  nome.trim();
  dia.trim();
  hEnt.trim();
  hSai.trim();

  int hi, mi, hf, mf;
  if (sscanf(hEnt.c_str(), "%d:%d", &hi, &mi) != 2) {
    Serial.println("[ERRO] hora entrada");
    return false;
  }
  if (sscanf(hSai.c_str(), "%d:%d", &hf, &mf) != 2) {
    Serial.println("[ERRO] hora saida");
    return false;
  }

  Serial.println("=== DADO RECEBIDO ===");
  Serial.println(rfid);
  Serial.println(nome);
  Serial.println(dia);
  Serial.println(hEnt);
  Serial.println(hSai);

  // evitar duplicar:
  if (!aluno_ja_existe(rfid)) {
    insere_linha_alunos(rfid, nome);
  } else {
    Serial.println("[AL] Aluno ja existe, nao vou inserir de novo.");
  }

  insere_horario(rfid, dia, hi, mi, hf, mf);
  Serial.println("[HR] Horario inserido (SEM checar duplicata).");

  // Atualiza estruturas para telas (apenas leitura do DB)
  // atualizarTurmaDeDB();
  // atualizarEstatDeDB();

  return true;
}

/* ---------------------------------------------------*/
/* ------------------- SETUP -------------------------*/
/* ---------------------------------------------------*/

extern unsigned int __heap_start, *__brkval;

int freeMemory() {
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

void setup() {
  Serial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(campainhaPassiva, OUTPUT);
  noTone(campainhaPassiva);

  //mem.clearAll();
  mem.init();
  cria_tabela_presenca();
  cria_tabela_alunos();
  cria_tabela_horarios();

  Wire.begin();

  uint16_t id = tela.readID();
  Serial.print("\nID LIDO: 0x");
  Serial.println(id, HEX);

  if (id == 0xD3D3 || id == 0xFFFF || id == 0x0000) id = 0x9486;
  tela.begin(id);
  tela.setRotation(0);
  tela.fillScreen(TFT_GREEN);

  if (!rtc.begin()) {
    tela.setTextColor(TFT_RED, TFT_BLACK);
    tela.setTextSize(2);
    tela.setCursor(20, 140);
    tela.println("ERRO RTC");
  } else {
    rtc.start();  // garante que o oscilador está rodando
    DateTime now = rtc.now();
    //rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    if (now.year() < 2024) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  // Inicializa estruturas de turma/estat com DB atual
  atualizarTurmaDeDB();
  //atualizarEstatDeDB();
  //atualizarEstatDeDB();
  //atualizarEstatDeDB();

  desenharTelaPadrao();
  desenharHoraData();

  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("RFID pronto. Aproxime o cartao...");
  Serial.setTimeout(2000);
}

/* ---------------------------------------------------*/
/* -------------------- LOOP -------------------------*/
/* ---------------------------------------------------*/

void loop() {
  // Serial.print("Memória livre: ");
  // Serial.println(freeMemory());
  unsigned long agoraMillis = millis();
  //int leituraAtual = digitalRead(BUTTON_PIN);

  if (exibindoMensagem) {
    // Controle de tempo da tela de ACESSO OK / NEGADO
    if (millis() - tempoInicioMensagem > duracaoMensagem) {
      exibindoMensagem = false;
      desenharTelaPadrao();
      desenharHoraData();
    }
  } else {
    // Atualiza relógio a cada 1s (na tela padrão)
    if (telaAtual == TELA_PADRAO && (millis() - ultimoUpdate >= 10000)) {
      ultimoUpdate = millis();

      DateTime dbg = rtc.now();
      Serial.print("RTC: ");
      Serial.print(dbg.hour());
      Serial.print(':');
      Serial.print(dbg.minute());
      Serial.print(':');
      Serial.println(dbg.second());
      desenharHoraData();
    }

    // Botão exportar PR
    // if (leituraAtual == LOW && ultimoEstadoBotao == HIGH &&
    //     (millis() - lastButtonPress > debounceDelay)) {
    //   exportar_tabela_presenca_single_line();
    //   lastButtonPress = millis();
    // }
    // ultimoEstadoBotao = leituraAtual;

    // ===== BLOCO DA SERIAL (mantido do código original) =====
    if (Serial.available()) {
      String entrada = Serial.readStringUntil('\n');  // LÊ UMA ÚNICA VEZ
      entrada.trim();

      if (entrada.length() == 0) {
        // linha vazia, não faz nada
      } else {

        Serial.print("[DBG] Entrada: '");
        Serial.print(entrada);
        Serial.println("'");

        // 1) Respostas de status do Python
        if (entrada.startsWith("ok:") || entrada.startsWith("nok:")) {
          int sep = entrada.indexOf(':');
          String tipo = entrada.substring(0, sep);
          String nome = entrada.substring(sep + 1);
          mostrarAcesso(tipo, nome);
        }

        // 2) Ajuste de hora (somente hora/min/seg)
        else if (entrada.startsWith("hora:")) {
          String timeString = entrada.substring(5);
          int newHour = 0, newMinute = 0, newSecond = 0;

          int itemsRead = sscanf(timeString.c_str(), "%d:%d:%d",
                                 &newHour, &newMinute, &newSecond);

          if (itemsRead >= 1) {
            DateTime now = rtc.now();
            DateTime newTime(now.year(), now.month(), now.day(),
                             newHour, newMinute, newSecond);
            rtc.adjust(newTime);
            Serial.println("OK: Horario ajustado com sucesso.");
            desenharHoraData();
          } else {
            Serial.println("ERRO: Formato de hora invalido.");
          }
        }

        // 3) Reset das tabelas
        else if (entrada.startsWith("reset")) {
          Serial.println("ATENÇÃO: Limpando tabelas!");
          limpar_tabela_alunos();
          limpar_tabela_horarios();
          limpar_tabela_presenca();
          atualizarTurmaDeDB();
          atualizarEstatDeDB();
          Serial.println("RESET_OK");
        }

        // 4) Exportar PR sob comando
        else if (entrada.equalsIgnoreCase("exportpr")) {
          exportar_tabela_presenca_single_line();
        }

        // 5) Dado:rfid,nome,dia,HH:MM,HH:MM
        else if (entrada.startsWith("Dado:") || entrada.startsWith("dado:")) {

          // Se a linha veio cortada (poucas vírgulas), tenta ler o resto
          while (contarVirgulas(entrada) < 4) {
            Serial.println("[DBG] Linha Dado incompleta, lendo mais...");
            String extra = Serial.readStringUntil('\n');
            extra.trim();
            if (extra.length() == 0) {
              // nada veio, evita loop infinito
              break;
            }
            Serial.print("[DBG] Complemento recebido: '");
            Serial.print(extra);
            Serial.println("'");
            entrada += extra;  // concatena o resto
          }

          bool ok = processa_dado_serial(entrada);
          if (ok) Serial.println("OK");
          else    Serial.println("ERRO");
        }


        // 6) Listagens
        else if (entrada.startsWith("listarAlunos")) {
          listar_tabela_alunos();
        }
        else if (entrada.startsWith("listarHorarios")) {
          listar_tabela_horarios();
        }
        else if (entrada.startsWith("listarPresenca")) {
          listar_tabela_presenca();
        }

        // 7) Qualquer outra coisa: trata como RFID vindo da Serial
        else {
          String rfid_tag = entrada;
          if (rfid_tag.length() > 0) {
            if (esta_no_horario_correto(rfid_tag)) {
              DateTime agora = rtc.now();
              registra_presenca_agora(rfid_tag, agora.hour(), agora.minute());
              atualizarTurmaDeDB();
              atualizarEstatDeDB();
              String nome = nomePorUID(rfid_tag);
              somPermitido();
              mostrarAcesso("ok", nome);
            } else {
              Serial.println("ACESSO NEGADO: Fora do horário agendado.");
              somNegado();
              mostrarAcesso("nok", rfid_tag);
            }
          }
        }
      }
    }

    // Leitor físico de cartão (somente na tela padrão para não pesar)
    static unsigned long ultimoRFID = 0;
    if (telaAtual == TELA_PADRAO &&
        (agoraMillis - ultimoRFID > 150)) {
      ultimoRFID = agoraMillis;
      checaCartaoRFID();
    }
  }

  // Processa botões do touch SEMPRE no final
  processarBotoes();
}