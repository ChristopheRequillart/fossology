/***************************************************************
 Copyright (C) 2006-2014 Hewlett-Packard Development Company, L.P.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 version 2 as published by the Free Software Foundation.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

 ***************************************************************/
/**
 * \file nomos.c
 * \brief Main for the nomos agent
 *
 * Nomos detects licenses and copyrights in a file.  Depending on how it is
 * invoked, it either stores it's findings in the FOSSology data base or
 * reports them to standard out.
 *
 */
/* CDB - What is this define for??? */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* not defined _GNU_SOURCE */

#include "nomos.h"
#include "util.h"
#include "list.h"
#include "licenses.h"
#include "process.h"
#include "nomos_regex.h"
#include "_autodefs.h"


#ifdef SVN_REV_S
char BuildVersion[]="nomos build version: " VERSION_S " r(" SVN_REV_S ").\n";
#else
char BuildVersion[]="nomos build version: NULL.\n";
#endif

extern licText_t licText[]; /* Defined in _autodata.c */
struct globals gl;
struct curScan cur;
int schedulerMode = 0; /**< Non-zero when being run from scheduler */
int Verbose = 0; 
FILE **pFile;
pid_t mainPid = 0; // main process id

/** shortname cache very simple nonresizing hash table */
struct cachenode 
{
  char *rf_shortname;
  long  rf_pk;
};
typedef struct cachenode cachenode_t;

struct cacheroot
{
  int maxnodes;
  cachenode_t *nodes;
};
typedef struct cacheroot cacheroot_t;

#define FUNCTION

char *getFieldValue(char *inStr, char *field, int fieldMax, char *value, int valueMax, char separator) ;
void  parseLicenseList() ;
void  Usage(char *Name) ;
void  Bail(int exitval) ;
int   optionIsSet(int val) ;
static void getFileLists(char *dirpath) ;
void  freeAndClearScan(struct curScan *thisScan) ;
void  processFile(char *fileToScan) ;
int   recordScanToDB(cacheroot_t *pcroot, struct curScan *scanRecord) ;
long  get_rfpk(cacheroot_t *pcroot, char *rf_shortname);

long  add2license_ref(char *licenseName) ;
int   updateLicenseFile(long rfPk) ;

int   initLicRefCache(cacheroot_t *pcroot) ;
long  lrcache_hash(cacheroot_t *pcroot, char *rf_shortname);
int   lrcache_add(cacheroot_t *pcroot, long rf_pk, char *rf_shortname);
long  lrcache_lookup(cacheroot_t *pcroot, char *rf_shortname);


/**
 add2license_ref
 \brief Add a new license to license_ref table

 Adds a license to license_ref table.

 @param  char *licenseName

 @return rf_pk for success, 0 for failure
 */
FUNCTION long add2license_ref(char *licenseName) {

  PGresult *result;
  char  query[myBUFSIZ];
  char  insert[myBUFSIZ];
  char  escLicName[myBUFSIZ];
  char *specialLicenseText;
  long rf_pk;

  int len;
  int error;
  int numRows;

  // escape the name
  len = strlen(licenseName);
  PQescapeStringConn(gl.pgConn, escLicName, licenseName, len, &error);
  if (error)
    LOG_WARNING("Does license name %s have multibyte encoding?", licenseName)

    /* verify the license is not already in the table */
    sprintf(query, "SELECT rf_pk FROM license_ref where rf_shortname='%s'", escLicName);
  result = PQexec(gl.pgConn, query);
  if (fo_checkPQresult(gl.pgConn, result, query, __FILE__, __LINE__)) return 0;
  numRows = PQntuples(result);
  if (numRows)
  {
    rf_pk = atol(PQgetvalue(result, 0, 0));
    PQclear(result);
    return rf_pk;
  }
  PQclear(result);

  /* Insert the new license */
  specialLicenseText = "License by Nomos.";

  sprintf( insert,
      "insert into license_ref(rf_shortname, rf_text, rf_detector_type) values('%s', '%s', 2)",
      escLicName, specialLicenseText);
  result = PQexec(gl.pgConn, insert);
  // ignore duplicate constraint failure (23505), report others
  if ((result==0) || ((PQresultStatus(result) != PGRES_COMMAND_OK) &&
      (strncmp("23505", PQresultErrorField(result, PG_DIAG_SQLSTATE),5))))
  {
    printf("ERROR: %s(%d): Nomos failed to add a new license. %s/n: %s/n",
        __FILE__,__LINE__, PQresultErrorMessage(result), insert);
    PQclear(result);
    return (0);
  }
  PQclear(result);

  /* retrieve the new rf_pk */
  result = PQexec(gl.pgConn, query);
  if (fo_checkPQresult(gl.pgConn, result, query, __FILE__, __LINE__)) return 0;
  numRows = PQntuples(result);
  if (numRows)
    rf_pk = atol(PQgetvalue(result, 0, 0));
  else
  {
    printf("ERROR: %s:%s:%d Just inserted value is missing. On: %s", __FILE__, "add2license_ref()", __LINE__, query);
    PQclear(result);
    return(0);
  }
  PQclear(result);

  return (rf_pk);
}


/**
 lrcache_hash

 \brief calculate the hash of an rf_shortname
 rf_shortname is the key

 @param cacheroot_t *
 @param rf_shortname

 @return hash value
 */
FUNCTION long lrcache_hash(cacheroot_t *pcroot, char *rf_shortname)
{
  long hashval = 0;
  int len, i;

  /* use the first sizeof(long) bytes for the hash value */
  len = (strlen(rf_shortname) < sizeof(long)) ? strlen(rf_shortname) : sizeof(long);
  for (i=0; i<len;i++) hashval += rf_shortname[i] << 8*i;
  hashval = hashval % pcroot->maxnodes;
  return hashval;
}


/**
 lrcache_print

 \brief Print the contents of the hash table

 @param cacheroot_t *

 @return none
 */
FUNCTION void lrcache_print(cacheroot_t *pcroot)
{
  cachenode_t *pcnode;
  long hashval = 0;
  int i;

  pcnode = pcroot->nodes;
  for (i=0; i<pcroot->maxnodes; i++)
  {
    if (pcnode->rf_pk != 0L) 
    {
      hashval = lrcache_hash(pcroot, pcnode->rf_shortname);
      printf("%ld, %ld, %s\n", hashval, pcnode->rf_pk, pcnode->rf_shortname);
    }
    pcnode++;
  }
}


/**
 lrcache_free

 \brief free the hash table

 @param cacheroot_t *

 @return none
 */
FUNCTION void lrcache_free(cacheroot_t *pcroot)
{
  cachenode_t *pcnode;
  int i;

  pcnode = pcroot->nodes;
  for (i=0; i<pcroot->maxnodes; i++)
  {
    if (pcnode->rf_pk != 0L) 
    {
      free(pcnode->rf_shortname);
    }
    pcnode++;
  }
  free(pcroot->nodes);
}


/**
 lrcache_add

 \brief add a rf_shortname, rf_pk to the license_ref cache 
 rf_shortname is the key

 @param cacheroot_t *
 @param rf_pk
 @param rf_shortname

 @return -1 for failure, 0 for success
 */
FUNCTION int lrcache_add(cacheroot_t *pcroot, long rf_pk, char *rf_shortname)
{
  cachenode_t *pcnode;
  long hashval = 0;
  int i;
  int noden;

  hashval = lrcache_hash(pcroot, rf_shortname);

  noden = hashval;
  for (i=0; i<pcroot->maxnodes; i++)
  {
    noden = (hashval +i) & (pcroot->maxnodes -1);

    pcnode = pcroot->nodes + noden;
    if (!pcnode->rf_pk)
    {
      pcnode->rf_shortname = strdup(rf_shortname);
      pcnode->rf_pk = rf_pk;
      break;
    }
  }
  if (i < pcroot->maxnodes) return 0;

  return -1;  /* no space */
}


/**
 lrcache_lookup

 \brief lookup rf_pk in the license_ref cache 
 rf_shortname is the key

 @param cacheroot_t *
 @param rf_shortname

 @return rf_pk, 0 if the shortname is not in the cache
 */
FUNCTION long lrcache_lookup(cacheroot_t *pcroot, char *rf_shortname)
{
  cachenode_t *pcnode;
  long hashval = 0;
  int i;
  int noden;

  hashval = lrcache_hash(pcroot, rf_shortname);

  noden = hashval;
  for (i=0; i<pcroot->maxnodes; i++)
  {
    noden = (hashval +i) & (pcroot->maxnodes -1);

    pcnode = pcroot->nodes + noden;
    if (!pcnode->rf_pk) return 0;
    if (strcmp(pcnode->rf_shortname, rf_shortname) == 0) 
    {
      return pcnode->rf_pk;
    }
  }

  return 0;  /* not found */
}



/**
 initLicRefCache

 \brief build a cache the license ref db table.

 @param cacheroot_t *

 initLicRefCache builds a cache using the rf_shortname as the key
 and the rf_pk as the value.  This is an optimization. The cache is used for 
 reference license lookups instead of querying the db.

 @return 0 for failure, 1 for success
 */

FUNCTION int initLicRefCache(cacheroot_t *pcroot) {

  PGresult *result;
  char query[myBUFSIZ];
  int row;
  int numLics;

  if (!pcroot) return 0;

  sprintf(query, "SELECT rf_pk, rf_shortname FROM license_ref where rf_detector_type=2;");
  result = PQexec(gl.pgConn, query);
  if (fo_checkPQresult(gl.pgConn, result, query, __FILE__, __LINE__)) return 0;

  numLics = PQntuples(result);
  /* populate the cache  */
  for (row = 0; row < numLics; row++)
  {
    lrcache_add(pcroot, atol(PQgetvalue(result, row, 0)), PQgetvalue(result, row, 1));
  }

  PQclear(result);

  return (1);
} /* initLicRefCache */


/**
 get_rfpk
 \brief Get the rf_pk for rf_shortname

 Checks the cache to get the rf_pk for this shortname.
 If it doesn't exist, add it to both license_ref and the 
 license_ref cache (the hash table).

 @param cacheroot_t *
 @param char *rf_shortname

 @return rf_pk of the matched license or 0
 */
FUNCTION long get_rfpk(cacheroot_t *pcroot, char *rf_shortname) {
  long  rf_pk;
  size_t len;

  if ((len = strlen(rf_shortname)) == 0) {
    printf("ERROR! Nomos.c get_rfpk() passed empty name");
    return (0);
  }

  /* is this in the cache? */
  rf_pk = lrcache_lookup(pcroot, rf_shortname);
  if (rf_pk) return rf_pk;

  /* shortname was not found, so add it */
  /* add to the license_ref table */
  rf_pk = add2license_ref(rf_shortname);

  /* add to the cache */
  lrcache_add(pcroot, rf_pk, rf_shortname);

  return (rf_pk);
} /* get_rfpk */

/**
 getFieldValue
 \brief Given a string that contains field='value' pairs, save the items.

 @return pointer to start of next field, or NULL at \0.

 \callgraph
 */
FUNCTION char *getFieldValue(char *inStr, char *field, int fieldMax, char *value,
    int valueMax, char separator) {
  int s;
  int f;
  int v;
  int gotQuote;

#ifdef PROC_TRACE
  traceFunc("== getFieldValue(inStr= %s fieldMax= %d separator= '%c'\n",
      inStr, fieldMax, separator);
#endif /* PROC_TRACE */

  memset(field, 0, fieldMax);
  memset(value, 0, valueMax);

  /* Skip initial spaces */
  while (isspace(inStr[0])) {
    inStr++;
  }

  if (inStr[0] == '\0') {
    return (NULL);
  }
  f = 0;
  v = 0;

  /* Skip to end of field name */
  for (s = 0; (inStr[s] != '\0') && !isspace(inStr[s]) && (inStr[s] != '='); s++) {
    field[f++] = inStr[s];
  }

  /* Skip spaces after field name */
  while (isspace(inStr[s])) {
    s++;
  }
  /* If it is not a field, then just return it. */
  if (inStr[s] != separator) {
    return (inStr + s);
  }
  if (inStr[s] == '\0') {
    return (NULL);
  }
  /* Skip '=' */
  s++;

  /* Skip spaces after '=' */
  while (isspace(inStr[s])) {
    s++;
  }
  if (inStr[s] == '\0') {
    return (NULL);
  }

  gotQuote = '\0';
  if ((inStr[s] == '\'') || (inStr[s] == '"')) {
    gotQuote = inStr[s];
    s++; /* skip quote */
    if (inStr[s] == '\0') {
      return (NULL);
    }
  }

  if (gotQuote) {
    for (; (inStr[s] != '\0') && (inStr[s] != gotQuote); s++) {
      if (inStr[s] == '\\') {
        value[v++] = inStr[++s];
      }
      else {
        value[v++] = inStr[s];
      }
    }
  }
  else {
    /* if it gets here, then there is no quote */
    for (; (inStr[s] != '\0') && !isspace(inStr[s]); s++) {
      if (inStr[s] == '\\') {
        value[v++] = inStr[++s];
      }
      else {
        value[v++] = inStr[s];
      }
    }
  }
  /* Skip spaces */
  while (isspace(inStr[s])) {
    s++;
  }

  return (inStr + s);
} /* getFieldValue */

/**
 parseLicenseList
 \brief parse the comma separated list of license names found
 Uses cur.compLic and sets cur.licenseList

 void?
 */

FUNCTION void parseLicenseList() {

  int numLics = 0;

  /* char saveLics[myBUFSIZ]; */
  char *saveptr = 0; /* used for strtok_r */
  char *saveLicsPtr;

  if ((strlen(cur.compLic)) == 0) {
    return;
  }

  /* check for a single name  FIX THIS!*/
  if (strstr(cur.compLic, ",") == NULL_CHAR) {
    cur.licenseList[0] = cur.compLic;
    cur.licenseList[1] = NULL;
    return;
  }

  saveLicsPtr = strcpy(saveLics, cur.compLic);

  cur.tmpLics = strtok_r(saveLicsPtr, ",", &saveptr);

  cur.licenseList[numLics] = cur.tmpLics;
  numLics++;

  saveLicsPtr = NULL;
  while (cur.tmpLics) {
    cur.tmpLics = strtok_r(saveLicsPtr, ",", &saveptr);
    if (cur.tmpLics == NULL) {
      break;
    }
    cur.licenseList[numLics] = cur.tmpLics;
    numLics++;
  }
  cur.licenseList[numLics] = NULL;
  numLics++;

  /*
     int i;
     for(i=0; i<numLics; i++){
     printf("cur.licenseList[%d] is:%s\n",i,cur.licenseList[i]);
     }

     printf("parseLicenseList: returning\n");
   */

  return;
} /* parseLicenseList */


FUNCTION void Usage(char *Name) {
  printf("Usage: %s [options] [file [file [...]]\n", Name);
  printf("  -h   :: help (print this message), then exit.\n");
  printf("  -i   :: initialize the database, then exit.\n");
  printf("  -c   :: specify the directory for the system configuration.\n");
  printf("  -l   :: print full file path (command line only).\n");
  printf("  -v   :: verbose (-vv = more verbose)\n");
  printf(
      "  file :: if files are listed, print the licenses detected within them.\n");
  printf("  no file :: process data from the scheduler.\n");
  printf("  -V   :: print the version info, then exit.\n");
  printf("  -d   :: specify a directory to scan.\n");
  printf("  -n   :: spaw n - 1 child processes to run, there will be n running processes(the parent and n - 1 children). \n the default n is 2(when n is less than 2 or not setting, will be changed to 2) when -d is specified.\n");
} /* Usage() */

FUNCTION void Bail(int exitval) {
#ifdef PROC_TRACE
  traceFunc("== Bail(%d)\n", exitval);
#endif /* PROC_TRACE */

#if defined(MEMORY_TRACING) && defined(MEM_ACCT)
  if (exitval) {
    memCacheDump("Mem-cache @ Bail() time:");
  }
#endif /* MEMORY_TRACING && MEM_ACCT */

  /* close database and scheduler connections */
  if (gl.pgConn) PQfinish(gl.pgConn);
  fo_scheduler_disconnect(exitval);
  exit(exitval);
}


FUNCTION int optionIsSet(int val) {
#ifdef PROC_TRACE
  traceFunc("== optionIsSet(%x)\n", val);
#endif /* PROC_TRACE */

  return (gl.progOpts & val);
} /* optionIsSet */

/**
 getFileLists
 \brief Initialize the lists: regular-files list cur.regfList and buffer-offset
 list cur.offList.

 \todo CDB - Could probably roll this back into the main processing
 loop or just put in a generic init func that initializes *all*
 the lists.

 \callgraph
 */

FUNCTION static void getFileLists(char *dirpath) {
#ifdef PROC_TRACE
  traceFunc("== getFileLists(%s)\n", dirpath);
#endif /* PROC_TRACE */

  /*    listInit(&gl.sarchList, 0, "source-archives list & md5sum map"); */
  listInit(&cur.regfList, 0, "regular-files list");
  listInit(&cur.offList, 0, "buffer-offset list");
#ifdef FLAG_NO_COPYRIGHT
  listInit(&gl.nocpyrtList, 0, "no-copyright list");
#endif /* FLAG_NO_COPYRIGHT */

  listGetItem(&cur.regfList, cur.targetFile);
  return;
} /* getFileLists */


/**
 * updateLicenseFile
 * \brief, insert rf_fk, agent_fk and pfile_fk into license_file table
 *
 * @param long rfPK the reference file foreign key
 *
 * returns boolean (True or False)
 *
 * \callgraph
 */
FUNCTION int updateLicenseFile(long rfPk) {

  PGresult *result;
  char query[myBUFSIZ];

  if (rfPk <= 0) {
    return (FALSE);
  }

  /* If files are comming from command line instead of fossology repo,
       then there are no pfiles.  So don't update the db
   */
  if (cur.cliMode == 1) return (TRUE);

  sprintf(query,
      "INSERT INTO license_file(rf_fk, agent_fk, pfile_fk) VALUES(%ld, %d, %ld)",
      rfPk, gl.agentPk, cur.pFileFk);

  result = PQexec(gl.pgConn, query);

  if ((PQresultStatus(result) != PGRES_COMMAND_OK) &&
      (strncmp("23505", PQresultErrorField(result, PG_DIAG_SQLSTATE),5)))
  {
    // ignoring duplicate constraint failure (23505)
    printf("ERROR: %s:%s:%d  Error:%s %s  On: %s",
        __FILE__, "updateLicenseFile()", __LINE__,
        PQresultErrorField(result, PG_DIAG_SQLSTATE),
        PQresultErrorMessage(result), query);
    PQclear(result);
    return (FALSE);
  }
  PQclear(result);
  return (TRUE);
} /* updateLicenseFile */

/**
 * freeAndClearScan
 * \brief Clean-up all the per scan data structures, freeing any old data.
 *
 * \callgraph
 */
FUNCTION void freeAndClearScan(struct curScan *thisScan) {
  /*
     Clear lists
   */
  listClear(&thisScan->regfList, DEALLOC_LIST);
  listClear(&thisScan->offList, DEALLOC_LIST);
  listClear(&thisScan->fLicFoundMap, DEALLOC_LIST);
  listClear(&thisScan->parseList, DEALLOC_LIST);
  listClear(&thisScan->lList, DEALLOC_LIST);

  /* remove keys, data and hash table */
  hdestroy();

}

/**
 * processFile
 * \brief process a single file
 *
 * \callgraph
 */
FUNCTION void processFile(char *fileToScan) {

  char *pathcopy;
#ifdef PROC_TRACE
  traceFunc("== processFile(%s)\n", fileToScan);
#endif /* PROC_TRACE */

  /* printf("   LOG: nomos scanning file %s.\n", fileToScan);  DEBUG */

  (void) strcpy(cur.cwd, gl.initwd);

  strcpy(cur.filePath, fileToScan);
  pathcopy = strdup(fileToScan);
  strcpy(cur.targetDir, dirname(pathcopy));
  free(pathcopy);
  strcpy(cur.targetFile, fileToScan);
  cur.targetLen = strlen(cur.targetDir);

  if (!isFILE(fileToScan)) {
    LOG_FATAL("\"%s\" is not a plain file", fileToScan)
                          Bail(-__LINE__);
  }

  getFileLists(cur.targetDir);
  listInit(&cur.fLicFoundMap, 0, "file-license-found map");
  listInit(&cur.parseList, 0, "license-components list");
  listInit(&cur.lList, 0, "license-list");

  processRawSource();

  /* freeAndClearScan(&cur); */
} /* Process File */

/**
 recordScanToDb

 Write out the information about the scan to the FOSSology database.

 curScan is passed as an arg even though it's available as a global,
 in order to facilitate future modularization of the code.

 Returns: 0 if successful, -1 if not.

 \callgraph
 */
FUNCTION int recordScanToDB(cacheroot_t *pcroot, struct curScan *scanRecord) {

  char *noneFound;
  long rf_pk;
  int  numLicenses;

#ifdef SIMULATESCHED
  /* BOBG: This allows a developer to simulate the scheduler
     with a load file for testing/debugging, without updating the
     database.  Like:
     cat myloadfile | ./nomos
     myloadfile is same as what scheduler sends:
     pfile_pk=311667 pfilename=9A96127E7D3B2812B50BF7732A2D0FF685EF6D6A.78073D1CA7B4171F8AFEA1497E4C6B33.183
     pfile_pk=311727 pfilename=B7F5EED9ECB679EE0F980599B7AA89DCF8FA86BD.72B00E1B419D2C83D1050C66FA371244.368
     etc.
   */
  printf("%s\n",scanRecord->compLic);
  return(0);
#endif

  noneFound = strstr(scanRecord->compLic, LS_NONE);
  if (noneFound != NULL)
  {
    rf_pk = get_rfpk(pcroot, "No_license_found");
    if (updateLicenseFile(rf_pk) == FALSE)  return (-1);
    return (0);
  }

  /* we have one or more license names, parse them */
  parseLicenseList();
  /* loop through the found license names */
  for (numLicenses = 0; cur.licenseList[numLicenses] != NULL; numLicenses++) {
    rf_pk = get_rfpk(pcroot, cur.licenseList[numLicenses]);
    if (rf_pk == 0) return(-1);
    if (updateLicenseFile(rf_pk) == FALSE)  return (-1);
  }
  return (0);
} /* recordScanToDb */

/**
 * \brief list all files(store file paths) in the specified directory
 *
 * \pamram dir_name - directory
 * \param process_count - process count
 */
void list_dir (const char * dir_name, int process_count)
{
  struct dirent *dp;
  DIR *dfd;
  int list_file_count = 0;
  const char *dir = dir_name;

  if ((dfd = opendir(dir)) == NULL)
  {
    fprintf(stderr, "Can't open %s\n", dir);
    return;
  }

  int i = 1;
  char *filename_qfd = NULL; // store one file path
  filename_qfd = (char *)malloc(MAX_FILE_PATH *sizeof(char));
  while ((dp = readdir(dfd)) != NULL)
  {
    struct stat stbuf ;

    /** allocte memory to store the path */
    while ((strlen(dir) + strlen(dp->d_name)) >= i*MAX_FILE_PATH)
    {
      filename_qfd = (char *)realloc(filename_qfd, (++i*MAX_FILE_PATH)*sizeof(char));
    }

    /* get the file path */
    sprintf( filename_qfd , "%s/%s",dir,dp->d_name) ;

    if( stat(filename_qfd,&stbuf ) == -1 )
    {
      LOG_FATAL("Unable to stat file: %s\n",filename_qfd) ;
      continue ;
    }

    if (strcmp (dp->d_name, "..") != 0 && strcmp (dp->d_name, ".") != 0)
    {
      if ((stbuf.st_mode & S_IFMT)  == S_IFDIR)
      {
        list_dir(filename_qfd, process_count);
      }
      else {
        sprintf(filename_qfd, "%s\n", filename_qfd);
        pid_t pid = getpid();
        int file_number = list_file_count%process_count;
        int size = fwrite (filename_qfd, sizeof(char), strlen(filename_qfd), pFile[file_number]);
        list_file_count++;

        if (process_count == list_file_count) list_file_count = 0; // reset list_file_count each cycle

        if (0 == size)
        LOG_NOTICE("file_number, pid, size, filename_qfd, process_count, list_file_count are:%d, %d, %s, %d, %d\n", pid, size, filename_qfd, process_count, list_file_count);
       continue;
      }
    }
  }
  filename_qfd = NULL;
  free(filename_qfd);
}

/** 
 * \brief read line by line, then call processFile to grab license line by line
 * 
 * \param file_number - while temp path file do you want to read and process
 */
void read_file_grab_license(int file_number)
{
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char path_file[TEMP_FILE_LEN] = "";

  sprintf(path_file, "/tmp/fossology_file_list_%d.txt", file_number);
  FILE *file = fopen(path_file, "r");
  if (!file) 
  {
    LOG_FATAL("failed to open %s\n", path_file);
    return;
  }
  while ((read = getline(&line, &len, file)) != -1) {
    if (line && line[0])
    {
      int lenth = strlen(line);
      while(isspace(line[lenth - 1])) line[--lenth] = 0;  // right trim
      while(isspace(*line)) ++line;  // left trim
    }
    //line = trim(line);
    processFile(line);
  }
  fclose(file);

  if (line) free(line);
}

/**
 * \brief the recursive create process and process grabbing licenses
 *
 * \param proc_num - how many child processes(proc_num - 1) will be created
 */
void myFork(int proc_num) {
  pid_t pid;
  pid = fork();

  if (pid < 0)
  {
    LOG_FATAL("fork failed\n");
  }
  else if (pid == 0) { // chile process, every singe process runs on one temp path file
    read_file_grab_license(proc_num); // grabbing licenses on /tmp/fossology_file_list_%d.txt
    return;
  }
  else if (pid > 0) {
    // if pid != 0, we're in the parent
    // let's call ourself again, decreasing the counter, until it reaches 1.
    if (proc_num > 1) {
      myFork(proc_num - 1);
    }
    else read_file_grab_license(0); // main(parent) process run on /tmp/fossology_file_list_%0.txt
  }
}

int main(int argc, char **argv) 
{
  int i;
  int c;
  int file_count = 0;
  int upload_pk = 0;
  int numrows;
  int ars_pk = 0;
  int user_pk = 0;
  char *AgentARSName = "nomos_ars";

  char *cp;
  char sErrorBuf[1024];
  char *agent_desc = "License Scanner";
  char **files_to_be_scanned; /**< The list of files to scan */
  char sqlbuf[1024];
  PGresult *result;
  char *SVN_REV = NULL;
  char *VERSION = NULL;
  char agent_rev[myBUFSIZ];
  cacheroot_t cacheroot;
  char *scanning_directory= NULL;
  int process_count = 0;

  /* connect to the scheduler */
  fo_scheduler_connect(&argc, argv, &(gl.pgConn));

#ifdef PROC_TRACE
  traceFunc("== main(%d, %p)\n", argc, argv);
#endif /* PROC_TRACE */

#ifdef MEMORY_TRACING
  mcheck(0);
#endif /* MEMORY_TRACING */
#ifdef GLOBAL_DEBUG
  gl.DEEBUG = gl.MEM_DEEBUG = 0;
#endif /* GLOBAL_DEBUG */

  files_to_be_scanned = calloc(argc, sizeof(char *));

  SVN_REV = fo_sysconfig("nomos", "SVN_REV");
  VERSION = fo_sysconfig("nomos", "VERSION");
  sprintf(agent_rev, "%s.%s", VERSION, SVN_REV);
  
  gl.agentPk = fo_GetAgentKey(gl.pgConn, basename(argv[0]), 0, agent_rev, agent_desc);

  /* Record the progname name */
  if ((cp = strrchr(*argv, '/')) == NULL_STR)
  {
    strncpy(gl.progName, *argv, sizeof(gl.progName));
  }
  else
  {
    while (*cp == '.' || *cp == '/') cp++;
    strncpy(gl.progName, cp, sizeof(gl.progName));
  }

  if (putenv("LANG=C") < 0)
  {
    strerror_r(errno, sErrorBuf, sizeof(sErrorBuf));
    LOG_FATAL("Cannot set LANG=C in environment.  Error: %s", sErrorBuf)
    Bail(-__LINE__);
  }

  /* Save the current directory */
  if (getcwd(gl.initwd, sizeof(gl.initwd)) == NULL_STR)
  {
    strerror_r(errno, sErrorBuf, sizeof(sErrorBuf));
    LOG_FATAL("Cannot obtain starting directory.  Error: %s", sErrorBuf)
    Bail(-__LINE__);
  }

  /* default paragraph size (# of lines to scan above and below the pattern) */
  gl.uPsize = 6;

  /* Build the license ref cache to hold 2**11 (2048) licenses.
       This MUST be a power of 2.
   */
  cacheroot.maxnodes = 2<<11;
  cacheroot.nodes = calloc(cacheroot.maxnodes, sizeof(cachenode_t));
  if (!initLicRefCache(&cacheroot))
  {
    LOG_FATAL("Nomos could not allocate %d cacheroot nodes.", cacheroot.maxnodes)
                          Bail(-__LINE__);
  }

  /* Process command line options */
  while ((c = getopt(argc, argv, "Vvhilc:d:n:")) != -1)
  {
    switch (c) {
      case 'c': break; /* handled by fo_scheduler_connect() */
      case 'i':
        /* "Initialize" */
        Bail(0); /* DB was opened above, now close it and exit */
      case 'l':
        /* set long command line output */
        gl.progOpts |= OPTS_LONG_CMD_OUTPUT;
        break;
      case 'v':
        Verbose++; break;
      case 'V':
        printf("%s", BuildVersion);
        Bail(0);
      case 'd': /* diretory to scan */
        scanning_directory = optarg;
        struct stat dir_sta;
        int ret = stat(scanning_directory, &dir_sta);
        if (-1 == ret || S_IFDIR != (dir_sta.st_mode & S_IFMT))
        {
          if (-1 == ret) printf("stat('%s') error message: %s.\n",scanning_directory, strerror(errno));
          else printf("Warning: '%s' from -d is not a good directory(dir_sta.st_mode & S_IFMT = %o).\n", scanning_directory, dir_sta.st_mode & S_IFMT);
          Usage(argv[0]);
          Bail(-__LINE__);
        }
        break;
      case 'n': /* spawn mutiple processes to scan */
        process_count = atoi(optarg);
        break;
      case 'h':
      default:
        Usage(argv[0]);
        Bail(-__LINE__);
    }
  }

  /* Copy filename args (if any) into array */
  for (i = optind; i < argc; i++)
  {
    files_to_be_scanned[file_count] = argv[i];
    file_count++;
  }

  licenseInit();
  gl.flags = 0;

  if (file_count == 0 && !scanning_directory)
  {
    char *repFile;

    /* We're being run from the scheduler */
    /* DEBUG printf("   LOG: nomos agent starting up in scheduler mode....\n"); */
    schedulerMode = 1;

    user_pk = fo_scheduler_userID(); /* get user_pk for user who queued the agent */

    /* read upload_pk from scheduler */
    while (fo_scheduler_next())
    {
      upload_pk = atoi(fo_scheduler_current());
      if (upload_pk == 0) continue;

      /* Check Permissions */
      if (GetUploadPerm(gl.pgConn, upload_pk, user_pk) < PERM_WRITE)
      {
        LOG_ERROR("You have no update permissions on upload %d", upload_pk);
        continue;
      }

      /* Is this a duplicate request (same upload_pk, sameagent_fk)?
             If so, there is no point in repeating it.
       */
      snprintf(sqlbuf, sizeof(sqlbuf),
          "select ars_pk from nomos_ars,agent \
                  where agent_pk=agent_fk and ars_success=true \
                    and upload_fk='%d' and agent_fk='%d'",
                    upload_pk, gl.agentPk);
      result = PQexec(gl.pgConn, sqlbuf);
      if (fo_checkPQresult(gl.pgConn, result, sqlbuf, __FILE__, __LINE__)) Bail(-__LINE__);
      if (PQntuples(result) != 0)
      {
        LOG_NOTICE("Ignoring requested nomos analysis of upload %d - Results are already in database.",
            upload_pk);
        PQclear(result);
        continue;
      }
      PQclear(result);

      /* Record analysis start in nomos_ars, the nomos audit trail. */
      ars_pk = fo_WriteARS(gl.pgConn, ars_pk, upload_pk, gl.agentPk, AgentARSName, 0, 0);

      /* retrieve the records to process */
      snprintf(sqlbuf, sizeof(sqlbuf),
          "SELECT pfile_pk, pfile_sha1 || '.' || pfile_md5 || '.' || pfile_size AS pfilename FROM (SELECT distinct(pfile_fk) AS PF FROM uploadtree WHERE upload_fk='%d' and (ufile_mode&x'3C000000'::int)=0) as SS left outer join license_file on (PF=pfile_fk and agent_fk='%d') inner join pfile on (PF=pfile_pk) WHERE fl_pk IS null or agent_fk <>'%d'",
          upload_pk, gl.agentPk, gl.agentPk);
      result = PQexec(gl.pgConn, sqlbuf);
      if (fo_checkPQresult(gl.pgConn, result, sqlbuf, __FILE__, __LINE__)) Bail(-__LINE__);
      numrows = PQntuples(result);

      /* process all files in this upload */
      for (i=0; i<numrows; i++)
      {
        strcpy(cur.pFile, PQgetvalue(result, i, 1));
        cur.pFileFk = atoi(PQgetvalue(result, i, 0));

        repFile = fo_RepMkPath("files", cur.pFile);
        if (!repFile)
        {
          LOG_FATAL("Nomos unable to open pfile_pk: %ld, file: %s", cur.pFileFk, cur.pFile);
          Bail(-__LINE__);
        }

        /* make sure this is a regular file, ignore if not */
        if (0 == isFILE(repFile)) continue;

        processFile(repFile);
        fo_scheduler_heart(1);
        if (recordScanToDB(&cacheroot, &cur))
        {
          LOG_FATAL("nomos terminating upload %d scan due to previous errors.",
              upload_pk);
          Bail(-__LINE__);
        }
        freeAndClearScan(&cur);
      }
      PQclear(result);

      /* Record analysis success in nomos_ars. */
      fo_WriteARS(gl.pgConn, ars_pk, upload_pk, gl.agentPk, AgentARSName, 0, 1);
    }
  }
  else
  { /******** Files on the command line ********/
    cur.cliMode = 1;
    //printf("process_count, scanning_directory are:%d, %s\n", process_count, scanning_directory);
    if (scanning_directory) {
      if (process_count < 2) process_count = 2; // the least count is 2, at least has one child process

      pFile = (FILE **)malloc(process_count*(sizeof(FILE)));
      int i = 0;
      char temp_path_file_name[TEMP_FILE_LEN] = ""; // store file path list
      for(i = 0; i < process_count; i++)
      {
        sprintf(temp_path_file_name, "/tmp/fossology_file_list_%d.txt", i);
        FILE *file = fopen(temp_path_file_name, "r");
        /** check if this file exists or not, delete when existing */
        if (file)
        {
          fclose(file);
          unlink(temp_path_file_name);
        }
        /** get the temp path file distriptors */
        pFile[i] = fopen(temp_path_file_name, "a");
        if (!pFile[i])
        {
          LOG_FATAL("failed to open %s\n", temp_path_file_name);
        }
      }
      /** walk through the specified directory to get all the file(file path) and */
      /** store into mutiple files(/tmp/fossology_file_list_%d.txt */
      /** /tmp/fossology_file_list_0.txt is used by the main process */
      list_dir(scanning_directory, process_count);

      /** after the walking through and strore job is done, close all the temp path file distriptors */
      for(i = 0; i < process_count; i++) fclose(pFile[i]);

      /** create process_count - 1 child processes(please do not forget we always have the main process) */
      if (process_count > 1) {
        mainPid = getpid(); // get main process id
        myFork(process_count - 1);
        int status = 0;
        pid_t wpid = 0;
        if (mainPid == getpid())
        {
          /** wait all processes done. */
          while(1){
            wpid = wait(&status);
            if (-1 == wpid) break;
          }

          /** delete the temp path files */
          for(i = 0; i < process_count; i++)
          {
            sprintf(temp_path_file_name, "/tmp/fossology_file_list_%d.txt", i);
            FILE *file = fopen(temp_path_file_name, "r");
            /** check if this file exists or not, delete when existing */
            if (file)
            {
              fclose(file);
              unlink(temp_path_file_name);
            }
          }
        }

        /** free memeory */
        pFile = NULL;
        free(pFile);
      }
    }
    else {
      if (0 != process_count) 
      {
        printf("Warning: -n {nprocs} ONLY works with -d {directory}.\n");
      }
      for (i = 0; i < file_count; i++) {
        processFile(files_to_be_scanned[i]);
        recordScanToDB(&cacheroot, &cur);
        freeAndClearScan(&cur);
      }
    }
  }

  lrcache_free(&cacheroot);  // for valgrind

  /* Normal Exit */
  Bail(0);

  /* this will never execute but prevents a compiler warning about reaching
     the end of a non-void function */
  return (0);
}
