#include "Marlin.h"
#include "cardreader.h"
#include "ultralcd.h"
#include "stepper.h"
#include "temperature.h"
#include "language.h"

#ifdef SDSUPPORT

CardReader::CardReader() {
  filesize = 0;
  sdpos = 0;
  sdprinting = false;
  cardOK = false;
  saving = false;
  logging = false;
  workDirDepth = 0;
  file_subcall_ctr = 0;
  memset(workDirParents, 0, sizeof(workDirParents));

  autostart_stilltocheck = true; //the SD start is delayed, because otherwise the serial cannot answer fast enough to make contact with the host software.
  autostart_index = 0;
  //power to SD reader
  #if SDPOWER > -1
    OUT_WRITE(SDPOWER, HIGH);
  #endif //SDPOWER

  next_autostart_ms = millis() + 5000;
}

char *createFilename(char *buffer, const dir_t &p) { //buffer > 12characters
  char *pos = buffer;
  for (uint8_t i = 0; i < 11; i++) {
    if (p.name[i] == ' ') continue;
    if (i == 8) *pos++ = '.';
    *pos++ = p.name[i];
  }
  *pos++ = 0;
  return buffer;
}

void CardReader::lsDive(const char *prepend, SdFile parent, const char * const match/*=NULL*/) {
  dir_t p;
  uint8_t cnt = 0;

  while (parent.readDir(p, longFilename) > 0) {
    if (DIR_IS_SUBDIR(&p) && lsAction != LS_Count && lsAction != LS_GetFilename) { // hence LS_SerialPrint
      char path[FILENAME_LENGTH*2];
      char lfilename[FILENAME_LENGTH];
      createFilename(lfilename, p);

      path[0] = 0;
      if (prepend[0] == 0) strcat(path, "/"); //avoid leading / if already in prepend
      strcat(path, prepend);
      strcat(path, lfilename);
      strcat(path, "/");

      //Serial.print(path);

      SdFile dir;
      if (!dir.open(parent, lfilename, O_READ)) {
        if (lsAction == LS_SerialPrint) {
          ECHO_LMV(ER, MSG_SD_CANT_OPEN_SUBDIR, lfilename);
        }
      }
      lsDive(path, dir);
      //close done automatically by destructor of SdFile
    }
    else {
      char pn0 = p.name[0];
      if (pn0 == DIR_NAME_FREE) break;
      if (pn0 == DIR_NAME_DELETED || pn0 == '.') continue;
      char lf0 = longFilename[0];
      if (lf0 == '.') continue;

      if (!DIR_IS_FILE_OR_SUBDIR(&p)) continue;

      filenameIsDir = DIR_IS_SUBDIR(&p);

      if (!filenameIsDir && (p.name[8] != 'G' || p.name[9] == '~')) continue;

      //if (cnt++ != nr) continue;
      createFilename(filename, p);
      if (lsAction == LS_SerialPrint) {
        ECHO_V(prepend);
        ECHO_EV(filename);
      }
      else if (lsAction == LS_Count) {
        nrFiles++;
      }
      else if (lsAction == LS_GetFilename) {
        if (match != NULL) {
          if (strcasecmp(match, filename) == 0) return;
        }
        else if (cnt == nrFiles) return;
        cnt++;
      }
    }
  }
}

void CardReader::ls()  {
  lsAction = LS_SerialPrint;
  root.rewind();
  lsDive("", root);
}

void CardReader::initsd() {
  cardOK = false;
  if (root.isOpen()) root.close();

  #ifdef SDSLOW
    #define SPI_SPEED SPI_HALF_SPEED
  #else
    #define SPI_SPEED SPI_FULL_SPEED
  #endif

  if (!card.init(SPI_SPEED,SDSS)
    #if defined(LCD_SDSS) && (LCD_SDSS != SDSS)
      && !card.init(SPI_SPEED, LCD_SDSS)
    #endif
  ) {
    ECHO_LM(ER, MSG_SD_INIT_FAIL);
  }
  else if (!volume.init(&card)) {
    ECHO_LM(ER, MSG_SD_VOL_INIT_FAIL);
  }
  else if (!root.openRoot(&volume)) {
    ECHO_LM(ER, MSG_SD_OPENROOT_FAIL);
  }
  else {
    cardOK = true;
    ECHO_LM(OK, MSG_SD_CARD_OK);
  }
  workDir = root;
  curDir = &root;
  
  /*if (!workDir.openRoot(&volume)) {
    ECHO_EM(MSG_SD_WORKDIR_FAIL);
  }*/
}

void CardReader::setroot() {
  /*if (!workDir.openRoot(&volume)) {
    ECHO_EM(MSG_SD_WORKDIR_FAIL);
  }*/
  workDir = root;
  curDir = &workDir;
}

void CardReader::release() {
  sdprinting = false;
  cardOK = false;
}

void CardReader::startFileprint() {
  if (cardOK) {
    sdprinting = true;
  }
}

void CardReader::pauseSDPrint() {
  if (sdprinting) sdprinting = false;
}

void CardReader::openLogFile(char* name) {
  logging = true;
  openFile(name, false);
}

void CardReader::getAbsFilename(char *t) {
  uint8_t cnt = 0;
  *t = '/'; t++; cnt++;
  for (uint8_t i = 0; i < workDirDepth; i++) {
    workDirParents[i].getFilename(t); //SDBaseFile.getfilename!
    while(*t && cnt < MAXPATHNAMELENGTH) { t++; cnt++; } //crawl counter forward.
  }
  if (cnt < MAXPATHNAMELENGTH - FILENAME_LENGTH)
    file.getFilename(t);
  else
    t[0] = 0;
}

void CardReader::openFile(char* name, bool read, bool replace_current/*=true*/, bool lcd_status/*=true*/) {
  if (!cardOK) return;
  if (file.isOpen()) { //replacing current file by new file, or subfile call
    if (!replace_current) {
      if (file_subcall_ctr > SD_PROCEDURE_DEPTH - 1) {
        ECHO_LMV(ER, MSG_SD_MAX_DEPTH, SD_PROCEDURE_DEPTH);
        kill();
        return;
      }
      //store current filename and position
      getAbsFilename(filenames[file_subcall_ctr]);
     
      filespos[file_subcall_ctr] = sdpos;
      file_subcall_ctr++;
    }
    file.close();
  }
  else { //opening fresh file
    file_subcall_ctr = 0; //resetting procedure depth in case user cancels print while in procedure
  }
  sdprinting = false;

  SdFile myDir;
  curDir = &root;
  char *fname = name;

  char *dirname_start, *dirname_end;
  if (name[0] == '/') {
    dirname_start = &name[1];
    while(dirname_start > 0) {
      dirname_end = strchr(dirname_start, '/');
      if (dirname_end > 0 && dirname_end > dirname_start) {
        char subdirname[FILENAME_LENGTH];
        strncpy(subdirname, dirname_start, dirname_end - dirname_start);
        subdirname[dirname_end - dirname_start] = 0;
        if (!myDir.open(curDir, subdirname, O_READ)) {
          ECHO_LMV(ER, MSG_SD_OPEN_FILE_FAIL, subdirname);
          return;
        }

        curDir = &myDir;
        dirname_start = dirname_end + 1;
      }
      else { // the remainder after all /fsa/fdsa/ is the filename
        fname = dirname_start;
        break;
      }
    }
  }
  else { //relative path
    curDir = &workDir;
  }

  if (read) {
    if (file.open(curDir, fname, O_READ)) {
      filesize = file.fileSize();
      ECHO_SMV(OK,MSG_SD_FILE_OPENED, fname);
      ECHO_EMV(MSG_SD_SIZE, filesize);
      sdpos = 0;
      getfilename(0, fname);
      if(lcd_status) lcd_setstatus(longFilename[0] ? longFilename : fname);
    }
    else {
      ECHO_LMV(ER,MSG_SD_OPEN_FILE_FAIL,fname);
    }
  }
  else { //write
    if (file.open(curDir, fname, O_CREAT | O_APPEND | O_WRITE | O_TRUNC)) {
      saving = true;
      ECHO_LMV(OK, MSG_SD_WRITE_TO_FILE, name);
      if(lcd_status) lcd_setstatus(fname);
    }
    else {
      ECHO_LMV(ER, MSG_SD_OPEN_FILE_FAIL,fname);
    }
  }
}

void CardReader::removeFile(char* name) {
  if (!cardOK) return;

  file.close();
  sdprinting = false;

  SdFile myDir;
  curDir = &root;
  char *fname = name;

  char *dirname_start, *dirname_end;
  if (name[0] == '/') {
    dirname_start = strchr(name, '/') + 1;
    while (dirname_start > 0) {
      dirname_end = strchr(dirname_start, '/');
      if (dirname_end > 0 && dirname_end > dirname_start) {
        char subdirname[FILENAME_LENGTH];
        strncpy(subdirname, dirname_start, dirname_end - dirname_start);
        subdirname[dirname_end - dirname_start] = 0;
        if (!myDir.open(curDir, subdirname, O_READ)) {
          ECHO_LMV(ER, MSG_SD_OPEN_FILE_FAIL, subdirname);
          return;
        }

        curDir = &myDir;
        dirname_start = dirname_end + 1;
      }
      else { // the remainder after all /fsa/fdsa/ is the filename
        fname = dirname_start;
        break;
      }
    }
  }
  else { // relative path
    curDir = &workDir;
  }

  if (file.remove(curDir, fname)) {
    ECHO_LMV(OK, MSG_SD_FILE_DELETED, fname);
    sdpos = 0;
  }
  else {
    ECHO_LMV(ER, MSG_SD_FILE_DELETION_ERR,fname);
  }
}

void CardReader::getStatus() {
  if (cardOK) {
    ECHO_SMV(OK, MSG_SD_PRINTING_BYTE, sdpos);
    ECHO_EMV(MSG_SD_SLASH, filesize);
  }
  else {
    ECHO_LM(OK, MSG_SD_NOT_PRINTING);
  }
}

void CardReader::write_command(char *buf) {
  char* begin = buf;
  char* npos = 0;
  char* end = buf + strlen(buf) - 1;

  file.writeError = false;
  if ((npos = strchr(buf, 'N')) != NULL) {
    begin = strchr(npos, ' ') + 1;
    end = strchr(npos, '*') - 1;
  }
  end[1] = '\r';
  end[2] = '\n';
  end[3] = '\0';
  file.write(begin);
  if (file.writeError) {
    ECHO_LM(ER, MSG_SD_ERR_WRITE_TO_FILE);
  }
}

void CardReader::checkautostart(bool force) {
  if (!force && (!autostart_stilltocheck || next_autostart_ms < millis()))
    return;

  autostart_stilltocheck = false;

  if (!cardOK) {
    initsd();
    if (!cardOK) return; // fail
  }

  char autoname[30];
  sprintf_P(autoname, PSTR("auto%i.g"), autostart_index);
  for (int8_t i = 0; i < (int8_t)strlen(autoname); i++) autoname[i] = tolower(autoname[i]);

  dir_t p;

  root.rewind();

  bool found = false;
  while (root.readDir(p, NULL) > 0) {
    for (int8_t i = 0; i < (int8_t)strlen((char*)p.name); i++) p.name[i] = tolower(p.name[i]);
    if (p.name[9] != '~' && strncmp((char*)p.name, autoname, 5) == 0) {
      char cmd[30];
      sprintf_P(cmd, PSTR("M23 %s"), autoname);
      enqueuecommand(cmd);
      enqueuecommands_P(PSTR("M24"));
      found = true;
    }
  }
  if (!found)
    autostart_index = -1;
  else
    autostart_index++;
}

void CardReader::closeFile(bool store_location) {
  file.sync();
  file.close();
  saving = logging = false;

  if (store_location) {
    //future: store printer state, filename and position for continuing a stopped print
    // so one can unplug the printer and continue printing the next day.
  }
}

/**
 * File parser for KEY->VALUE format from files
 *
 * Author: Simone Primarosa
 *
 */

void CardReader::parseKeyLine(char *key, char *value, int &len_k, int &len_v) {
  if (!cardOK || !isFileOpen()) {
    key[0] = value[0] = '\0';
    len_k = len_v = 0;
    return;
  }
  int ln_buf = 0;
  char ln_char;
  bool ln_space = false, ln_ignore = false, key_found = false;
  while(!eof()) {		//READ KEY
    ln_char = (char)get();
    if(ln_char == '\n') {
      ln_buf = 0;
      ln_ignore = false;  //We've reached a new line try to find a key again
      continue;
    }
    if(ln_ignore) continue;
    if(ln_char == ' ') {
      ln_space = true;
      continue;
    }
    if(ln_char == '=') {
      key[ln_buf] = '\0';
      len_k = ln_buf;
      key_found = true;
      break; //key finded and buffered
    }
    if(ln_char == ';' || ln_buf+1 >= len_k || ln_space && ln_buf > 0) { //comments on key is not allowd. Also key len can't be longer than len_k or contain spaces. Stop buffering and try the next line
      ln_ignore = true;
      continue;
    }
    ln_space = false;
    key[ln_buf] = ln_char;
    ln_buf++;
  }
  if(!key_found) { //definitly there isn't no more key that can be readed in the file
    key[0] = value[0] = '\0';
    len_k = len_v = 0;
    return;
  }
  ln_buf = 0;
  ln_ignore = false;
  while(!eof()) {		//READ VALUE
    ln_char = (char)get();
	  if(ln_char == '\n') {
      value[ln_buf] = '\0';
      len_v = ln_buf;
      break;	//new line reached, we can stop
    }
    if(ln_ignore|| ln_char == ' ' && ln_buf == 0) continue; //ignore also initial spaces of the value
    if(ln_char == ';' || ln_buf+1 >= len_v) { //comments reached or value len longer than len_v. Stop buffering and go to the next line.
      ln_ignore = true;
      continue;
    }
    value[ln_buf] = ln_char;
    ln_buf++;
  }
}

void CardReader::unparseKeyLine(const char *key, char *value) {
  if (!cardOK || !isFileOpen()) return;
  file.writeError = false;
  file.write(key);
  if (file.writeError) {
    ECHO_LM(ER, MSG_SD_ERR_WRITE_TO_FILE);
    return;
  }
  
  file.writeError = false;
  file.write("=");
  if (file.writeError) {
    ECHO_LM(ER, MSG_SD_ERR_WRITE_TO_FILE);
    return;
  }
  
  file.writeError = false;
  file.write(value);
  if (file.writeError) {
    ECHO_LM(ER, MSG_SD_ERR_WRITE_TO_FILE);
    return;
  }

  file.writeError = false;
  file.write("\n");
  if (file.writeError) {
    ECHO_LM(ER, MSG_SD_ERR_WRITE_TO_FILE);
    return;
  }
}

/**
 * Get the name of a file in the current directory by index
 */
void CardReader::getfilename(uint16_t nr, const char * const match/*=NULL*/) {
  curDir = &workDir;
  lsAction = LS_GetFilename;
  nrFiles = nr;
  curDir->rewind();
  lsDive("", *curDir, match);
}

uint16_t CardReader::getnrfilenames() {
  curDir = &workDir;
  lsAction = LS_Count;
  nrFiles = 0;
  curDir->rewind();
  lsDive("", *curDir);
  return nrFiles;
}

void CardReader::chdir(const char * relpath) {
  SdFile newfile;
  SdFile *parent = &root;

  if (workDir.isOpen()) parent = &workDir;

  if (newfile.open(*parent, relpath, O_READ)) {
    if (workDirDepth < MAX_DIR_DEPTH) {
      workDirDepth++;
      for (int d = workDirDepth; d--;) workDirParents[d + 1] = workDirParents[d];
      workDirParents[0] = *parent;
    }
    workDir = newfile;
  }
  else {
    ECHO_LMV(ER, MSG_SD_CANT_ENTER_SUBDIR, relpath);
  }
}

void CardReader::updir() {
  if (workDirDepth > 0) {
    workDirDepth--;
    workDir = workDirParents[0];
    for (uint16_t d = 0; d < workDirDepth; d++)
      workDirParents[d] = workDirParents[d+1];
  }
}

void CardReader::printingHasFinished() {
  st_synchronize();
  if (file_subcall_ctr > 0) { // Heading up to a parent file that called current as a procedure.
    file.close();
    file_subcall_ctr--;
    openFile(filenames[file_subcall_ctr], true, true);
    setIndex(filespos[file_subcall_ctr]);
    startFileprint();
  }
  else {
    file.close();
    sdprinting = false;
    if (SD_FINISHED_STEPPERRELEASE) {
      //finishAndDisableSteppers();
      enqueuecommands_P(PSTR(SD_FINISHED_RELEASECOMMAND));
    }
    autotempShutdown();
  }
}

#endif //SDSUPPORT
