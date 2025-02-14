// LIGHTFALL: an EZ2AC Final EX score tracking hook
// 

#include "lightfall.h"
#include "sqlite3.h"
#include <shlwapi.h>

Logger logger;
uintptr_t scoreArrayAddr;
sqlite3* db;
sqlite3_stmt* stmt;

void saveScore(scoredata_t &scoredata) {
	if (sqlite3_bind_text(stmt, 1, scoredata.title, -1, 0) != SQLITE_OK) {
		logger.log("Error binding variable to insert statement: " + std::string(sqlite3_errmsg(db)) + "\n");
	}
	sqlite3_bind_int(stmt, 2, scoredata.mode);
	sqlite3_bind_text(stmt, 3, scoredata.difficulty, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 4, scoredata.level);
	sqlite3_bind_int(stmt, 5, scoredata.score);
	sqlite3_bind_int(stmt, 6, scoredata.rate);
	sqlite3_bind_text(stmt, 7, scoredata.grade, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 8, scoredata.total_notes);
	sqlite3_bind_int(stmt, 9, scoredata.kool);
	sqlite3_bind_int(stmt, 10, scoredata.cool);
	sqlite3_bind_int(stmt, 11, scoredata.good);
	sqlite3_bind_int(stmt, 12, scoredata.miss);
	sqlite3_bind_int(stmt, 13, scoredata.fail);
	sqlite3_bind_int(stmt, 14, scoredata.max_combo);
	sqlite3_bind_text(stmt, 15, scoredata.random_op, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 16, scoredata.auto_op, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 17, scoredata.stage);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		logger.log("Error saving score to database: " + std::string(sqlite3_errmsg(db)) + "\n");
	}
	else {
		logger.log("Score saved for song: " + std::string(scoredata.title) + "\n");
	}
	if (sqlite3_reset(stmt) != SQLITE_OK) {
		logger.log("Error resetting insert statement: " + std::string(sqlite3_errmsg(db)) + "\n");
	}
}

// stdcall to clean up stack from within function, don't have to do it in asm
void __stdcall getResultsScreenData() {
	if (readInt(battleAddr) != 0) {
		logger.log("In battle mode, skipping score\n");
		return;
	}

	scoredata_t scoredata = {};
	scoredata.mode = readInt(modeAddr);
	readScoreArray(scoreArrayAddr, scoredata);
	// 0-3 for stages 1-4, some values are offset by it
	// Saving this mainly for keeping track of song order in courses
	scoredata.stage = readInt(stageAddr);
	scoredata.rate = readInt(rateAddr + (scoredata.stage * 4));
	scoredata.level = readInt(levelAddr);

	if (scoredata.mode == 12) { // Separate title logic for CV2
		readString(scoredata.title, cv2SongNameAddr, 32);
	}
	else {
		char discName[128];
		readString(discName, discNameAddr + (scoredata.stage * 128), 128);
		// Disc name formatted either as title or title-diff if difficulty is above NM
		// String slicing to extract data
		size_t dashPos = strcspn(discName, "-");
		if (dashPos != strlen(discName)) {
			strncpy_s(scoredata.title, discName, dashPos);
			strncpy_s(scoredata.difficulty, discName + dashPos + 1, 3);
		}
		else {
			strcpy_s(scoredata.title, discName);
			strcpy_s(scoredata.difficulty, "nm");
		}
	}
	if (6 <= scoredata.mode && scoredata.mode <= 9) { // Checking if in course mode
		readString(scoredata.difficulty, courseNameAddr, 128);
	}

	// Getting random + auto settings
	bool randomFound = false;
	for (int i = 0; i < 11; i++) {
		if (readInt(randomAddrs[i])) {
			randomFound = true;
			strcpy_s(scoredata.random_op, randomNames[i]);
			break;
		}
	}
	if (randomFound == false) {
		strcpy_s(scoredata.random_op, "OFF");
	}
	// 5K Only, Catch, Turntable and CV2 don't have auto options
	if (scoredata.mode == 0 || scoredata.mode == 10 || 
		scoredata.mode == 11 || scoredata.mode == 12) {
		strcpy_s(scoredata.auto_op, "OFF");
	}
	// 14K has different auto options
	else if (scoredata.mode == 5 || scoredata.mode == 9) {
		strcpy_s(scoredata.auto_op, autoOps14K[readInt(autoAddr)]);
	}
	else {
		strcpy_s(scoredata.auto_op, autoOps[readInt(autoAddr)]);
	}

	saveScore(scoredata);
}

// declspec(naked) stops function prologue/epilogue from generating
__declspec(naked) void resScreenDetour() {
	__asm {
		mov scoreArrayAddr, edi; // edi has pointer we want
		pushad;
		pushfd; // Pushing registers and flags to restore them after function
		call getResultsScreenData;
		popfd;
		popad;
		mov eax, ecx; // Instructions that were replaced
		shl eax, 4; 
		jmp resScreenJumpBackAddr;
	}
}

int dbInit(char dbPath[]) {
	int rc;
	rc = sqlite3_open(dbPath, &db);
	if (rc != SQLITE_OK) {
		logger.log("Error opening database: " + std::string(sqlite3_errmsg(db)) + "\n");
		return 1;
	}

	char* sql =
		"CREATE TABLE IF NOT EXISTS score ("
		"id INTEGER PRIMARY KEY,"
		"time TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP," // UTC ISO-8601 timestamp
		"title TEXT NOT NULL,"
		"mode INTEGER NOT NULL,"
		"difficulty TEXT,"
		"level INTEGER,"
		"score INTEGER NOT NULL,"
		"rate INTEGER,"
		"grade TEXT,"
		"total_notes INTEGER,"
		"kool INTEGER,"
		"cool INTEGER,"
		"good INTEGER,"
		"miss INTEGER,"
		"fail INTEGER,"
		"max_combo INTEGER,"
		"random TEXT,"
		"auto TEXT,"
		"stage INTEGER);";
	char* errormsg;
	rc = sqlite3_exec(db, sql, NULL, NULL, &errormsg);
	if (rc != SQLITE_OK) {
		logger.log("Error writing to database: " + std::string(errormsg) + "\n");
		sqlite3_free(errormsg);
		sqlite3_close(db);
		return 1;
	}
	
	sql = // Preparing insert statement
		"INSERT INTO score(title, mode, difficulty, level, score, rate, grade, "
		"total_notes, kool, cool, good, miss, fail, max_combo, random, auto, stage) "
		"VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
	rc = sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL);
	if (rc != SQLITE_OK) {
		logger.log("Error preparing database insert statement: " + std::string(sqlite3_errmsg(db)) + "\n");
		sqlite3_close(db);
		return 1;
	}
	else {
		logger.log("Database opened successfully\n");
	}
	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH: {
		LPCSTR config = ".\\lightfall.ini";
		if (GetPrivateProfileIntA("Settings", "DisableLogging", 0, config)) {
			logger.disabled = true;
		}

		char dbPath[MAX_PATH];
		GetPrivateProfileStringA("Settings", "SavePath", NULL, dbPath, MAX_PATH, config);
		if (dbPath != NULL) {
			char buff[MAX_PATH];
			strcpy_s(buff, dbPath);
			PathAppendA(buff, "lightfall.log");
			logger.open(buff);
			PathAppendA(dbPath, "scores.db");
		}
		else {
			logger.open("lightfall.log");
			strcpy_s(dbPath, "scores.db");
		}
		logger.log("Starting up...\n");

		LPCSTR twoezconfig = ".\\2EZ.ini";
		// Short sleep to fix crash when using legitimate data with dongle, can be overriden in 2EZ.ini if causing issues.
		Sleep(GetPrivateProfileIntA("Settings", "ShimDelay", 10, twoezconfig));
		if (GetPrivateProfileIntA("Settings", "GameVer", 21, twoezconfig) != 21) {
			logger.log("Game version not Final: EX, stopping\n");
			break;
		}
		if (dbInit(dbPath) != 0) {
			break;
		}

		// Parsing the results screen is easiest by inserting hook in the middle of the function,
		// where a register temporarily has pointer to important score values
		// Something like MinHook or Detours couldn't easily be used, so I manually patched in a jump instead
		patchJump(resScreenJumpAddr, (LPBYTE)resScreenDetour);
		
		std::ostringstream patchAddr;
		patchAddr << std::uppercase << std::hex << resScreenJumpAddr;
		logger.log("Hook created at 0x" + patchAddr.str() + ", ready to save scores\n");
		break;
	}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

