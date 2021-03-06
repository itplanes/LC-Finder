﻿/* ***************************************************************************
* finder.c -- main code of LC-Finder, responsible for the initialization of 
* the LC-Finder and the scheduling of other functions.
*
* Copyright (C) 2016 by Liu Chao <lc-soft@live.cn>
*
* This file is part of the LC-Finder project, and may only be used, modified,
* and distributed under the terms of the GPLv2.
*
* By continuing to use, modify, or distribute this file you indicate that you
* have read the license and understand and accept it fully.
*
* The LC-Finder project is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
* or FITNESS FOR A PARTICULAR PURPOSE. See the GPL v2 for more details.
*
* You should have received a copy of the GPLv2 along with this file. It is
* usually in the LICENSE.TXT file, If not, see <http://www.gnu.org/licenses/>.
* ****************************************************************************/

/* ****************************************************************************
* finder.c -- LC-Finder 主程序代码，负责整个程序的初始化和其它功能的调度。
*
* 版权所有 (C) 2016 归属于 刘超 <lc-soft@live.cn>
*
* 这个文件是 LC-Finder 项目的一部分，并且只可以根据GPLv2许可协议来使用、更改和
* 发布。
*
* 继续使用、修改或发布本文件，表明您已经阅读并完全理解和接受这个许可协议。
*
* LC-Finder 项目是基于使用目的而加以散布的，但不负任何担保责任，甚至没有适销
* 性或特定用途的隐含担保，详情请参照GPLv2许可协议。
*
* 您应已收到附随于本文件的GPLv2许可协议的副本，它通常在 LICENSE 文件中，如果
* 没有，请查看：<http://www.gnu.org/licenses/>.
* ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <direct.h>
#define mkdir(PATH) _wmkdir(PATH)
#endif
#include "finder.h"
#include "ui.h"
#include <LCUI/font/charset.h>

#define EncodeUTF8(STR, WSTR, LEN) LCUI_EncodeString( STR, WSTR, LEN, ENCODING_UTF8 )

Finder finder;

typedef struct DirStatusDataPackRec_ {
	FileSyncStatus status;
	DB_Dir dir;
} DirStatusDataPackRec, *DirStatusDataPack;

typedef struct EventPackRec_ {
	EventHandler handler;
	void *data;
} EventPackRec, *EventPack;

static void OnEvent( LCUI_Event e, void *arg )
{
	EventPack pack = e->data;
	pack->handler( pack->data, arg );
}

int LCFinder_BindEvent( int event_id, EventHandler handler, void *data )
{
	EventPack pack = NEW( EventPackRec, 1 );
	pack->handler = handler;
	pack->data = data;
	return EventTrigger_Bind( finder.trigger, event_id, OnEvent, pack, free );
}

int LCFinder_TriggerEvent( int event_id, void *data )
{
	return  EventTrigger_Trigger( finder.trigger, event_id, data );
}

DB_Dir LCFinder_GetDir( const char *dirpath )
{
	int i;
	DB_Dir dir;
	for( i = 0; i < finder.n_dirs; ++i ) {
		dir = finder.dirs[i];
		if( dir && strcmp(dir->path, dirpath) == 0 ) {
			return dir;
		}
	}
	return NULL;
}

static wchar_t *LCFinder_CreateThumbDB( const char *dirpath )
{
	int len;
	ThumbDB db;
	wchar_t *wpath;
	char dbpath[PATH_LEN], path[PATH_LEN], name[44];

	strcpy( path, dirpath );
	EncodeSHA1( name, path, strlen(path) );
	EncodeUTF8( dbpath, finder.thumbs_dir, PATH_LEN );
	len = pathjoin( dbpath, dbpath, name ) + 1;
	db = ThumbDB_Open( dbpath );
	Dict_Add( finder.thumb_dbs, path, db );
	wpath = malloc( sizeof( wchar_t )*len );
	LCUI_DecodeString( wpath, dbpath, len, ENCODING_UTF8 );
	return wpath;
}

DB_Dir LCFinder_AddDir( const char *dirpath )
{
	int i, len;
	char *path;
	wchar_t **paths;
	DB_Dir dir, *dirs;
	len = strlen( dirpath );
	path = malloc( (len + 2) * sizeof( char ) );
	strcpy( path, dirpath );
	if( path[len - 1] != PATH_SEP ) {
		path[len] = PATH_SEP;
		path[len + 1] = 0;
	}
	for( i = 0; i < finder.n_dirs; ++i ) {
		dir = finder.dirs[i];
		if( dir && strstr( dir->path, path ) ) {
			free( path );
			return NULL;
		}
	}
	dir = DB_AddDir( dirpath );
	if( !dir ) {
		free( path );
		return NULL;
	}
	i = finder.n_dirs;
	finder.n_dirs += 1;
	dirs = realloc( finder.dirs, sizeof( DB_Dir )*finder.n_dirs );
	if( !dirs ) {
		finder.n_dirs -= 1;
		return NULL;
	}
	paths = realloc( finder.thumb_paths, 
			 sizeof( wchar_t* )*finder.n_dirs );
	if( !path ) {
		finder.n_dirs -= 1;
		return NULL;
	}
	dirs[i] = dir;
	paths[i] = LCFinder_CreateThumbDB( dir->path );
	finder.dirs = dirs;
	finder.thumb_paths = paths;
	return dir;
}

void LCFinder_DeleteDir( DB_Dir dir )
{
	int i, len;
	SyncTask t;
	ThumbDB db;
	wchar_t *wpath;
	for( i = 0; i < finder.n_dirs; ++i ) {
		if( dir == finder.dirs[i] ) {
			break;
		}
	}
	if( i >= finder.n_dirs ) {
		return;
	}
	finder.dirs[i] = NULL;
	len = strlen( dir->path ) + 1;
	wpath = NEW( wchar_t, len );
	LCUI_DecodeString( wpath, dir->path, len, ENCODING_UTF8 );
	/* 准备清除文件列表缓存 */
	t = SyncTask_NewW( finder.fileset_dir, wpath );
	LCFinder_TriggerEvent( EVENT_DIR_DEL, dir );
	SyncTask_ClearCache( t );
	free( wpath );
	/* 准备清除缩略图数据库 */
	wpath = finder.thumb_paths[i];
	finder.thumb_paths[i] = NULL;
	db = Dict_FetchValue( finder.thumb_dbs, dir->path );
	if( db ) {
		Dict_Delete( finder.thumb_dbs, dir->path );
		ThumbDB_Close( db );
		_wremove( wpath );
	}
	/* 删除数据库中的源文件夹记录 */
	DB_DeleteDir( dir );
	free( dir->path );
	free( dir );
}

static void SyncAddedFile( void *data, const wchar_t *wpath )
{
	static char path[PATH_LEN];
	DirStatusDataPack pack = data;
	int ctime = wgetfilectime( wpath );
	pack->status->synced_files += 1;
	LCUI_EncodeString( path, wpath, PATH_LEN, ENCODING_UTF8 );
	DB_AddFile( pack->dir, path, ctime );
	//wprintf(L"sync: add file: %s, ctime: %d\n", wpath, ctime);
}

static void SyncDeletedFile( void *data, const wchar_t *wpath )
{
	static char path[PATH_LEN];
	DirStatusDataPack pack = data;
	pack->status->synced_files += 1;
	LCUI_EncodeString( path, wpath, PATH_LEN, ENCODING_UTF8 );
	DB_DeleteFile( pack->dir, path );
	//wprintf(L"sync: delete file: %s\n", wpath);
}

DB_Dir LCFinder_GetSourceDir( const char *filepath )
{
	int i;
	DB_Dir dir;
	for( i = 0; i < finder.n_dirs; ++i ) {
		dir = finder.dirs[i];
		if( dir && strstr(filepath, dir->path) == filepath ) {
			return dir;
		}
	}
	return NULL;
}

int64_t LCFinder_GetThumbDBTotalSize( void )
{
	int i;
	int64_t sum_size;
	for( i = 0, sum_size = 0; i < finder.n_dirs; ++i ) {
		if( finder.thumb_paths[i] ) {
			sum_size += wgetfilesize( finder.thumb_paths[i] );
		}
	}
	return sum_size;
}

int LCFinder_SyncFiles( FileSyncStatus s )
{
	int i, len;
	DB_Dir dir;
	wchar_t *path;
	s->task = NULL;
	s->tasks = NULL;
	s->added_files = 0;
	s->synced_files = 0;
	s->scaned_files = 0;
	s->deleted_files = 0;
	s->state = STATE_STARTED;
	s->tasks = NEW( SyncTask, finder.n_dirs );
	for( i = 0; i < finder.n_dirs; ++i ) {
		dir = finder.dirs[i];
		if( !dir ) {
			continue;
		}
		len = strlen( dir->path ) + 1;
		path = malloc( sizeof( wchar_t )*len );
		len = LCUI_DecodeString( path, dir->path, len, ENCODING_UTF8 );
		path[len] = 0;
		s->task = SyncTask_NewW( finder.fileset_dir, path );
		SyncTask_Start( s->task );
		s->added_files += s->task->added_files;
		s->scaned_files += s->task->total_files;
		s->deleted_files += s->task->deleted_files;
		s->tasks[i] = s->task;
		free( path );
	}
	DB_Begin();
	s->state = STATE_SAVING;
	wprintf(L"\n\nstart sync\n");
	for( i = 0; i < finder.n_dirs; ++i ) {
		DirStatusDataPackRec pack;
		pack.dir = finder.dirs[i];
		if( !pack.dir ) {
			continue;
		}
		pack.status = s;
		s->task = s->tasks[i];
		SyncTask_InAddedFiles( s->task, SyncAddedFile, &pack );
		SyncTask_InDeletedFiles( s->task, SyncDeletedFile, &pack );
		SyncTask_Commit( s->task );
		SyncTask_Delete( &s->task );
	}
	DB_Commit();
	wprintf(L"\n\nend sync\n");
	s->state = STATE_FINISHED;
	s->task = NULL;
	free( s->tasks );
	s->tasks = NULL;
	return s->synced_files;
}

#ifdef _WIN32

static void InitConsoleWindow( void )
{
	int hCrt;
	FILE *hf;
	AllocConsole();
	hCrt = _open_osfhandle( (long)GetStdHandle( STD_OUTPUT_HANDLE ), _O_TEXT );
	hf = _fdopen( hCrt, "w" );
	*stdout = *hf;
	setvbuf( stdout, NULL, _IONBF, 0 );
	printf( "InitConsoleWindow OK!\n" );
}

#endif

/** 初始化工作目录 */
static void LCFinder_InitWorkDir( void )
{
	int len;
	wchar_t data_dir[1024];
	wchar_t *dirs[2] = {L"fileset", L"thumbs"};
	/* 如果要调试此程序，需手动设置程序所在目录 */
	//_wchdir( L"F:\\LC-Soft\\LC-Finder" );
	wgetcurdir( data_dir, 1024 );
	wprintf(L"data_dir: %s\n", data_dir);
	wpathjoin( data_dir, data_dir, L"data" );
	len = wcslen( data_dir );
	if( data_dir[len - 1] != PATH_SEP ) {
		data_dir[len++] = PATH_SEP;
		data_dir[len] = 0;
	}
	finder.data_dir = NEW( wchar_t, len + 2 );
	finder.fileset_dir = NEW( wchar_t, len + 2 + wcslen(dirs[0]) );
	finder.thumbs_dir = NEW( wchar_t, len + 2 +wcslen(dirs[1]) );
	wsprintf( finder.fileset_dir, L"%s%s", data_dir, dirs[0] );
	wsprintf( finder.thumbs_dir, L"%s%s", data_dir, dirs[1] );
	wcscpy( finder.data_dir, data_dir );
	mkdir( finder.data_dir );
	mkdir( finder.fileset_dir );
	mkdir( finder.thumbs_dir );
	_wsetlocale(LC_ALL, L"chs");
}

/** 初始化文件数据库 */
static void LCFInder_InitFileDB( void )
{
	DB_Init();
	finder.n_dirs = DB_GetDirs( &finder.dirs );
	finder.n_tags = DB_GetTags( &finder.tags );
}

static void ThumbDBDict_ValDel( void *privdata, void *val )
{
	ThumbDB_Close( val );
}

/** 初始化缩略图数据库 */
static void LCFinder_InitThumbDB( void )
{
	int i;
	wchar_t *path;
	printf("[thumbdb] init ...\n");
	finder.thumb_dbs = StrDict_Create( NULL, ThumbDBDict_ValDel );
	finder.thumb_paths = malloc( sizeof( wchar_t* )*finder.n_dirs );
	for( i = 0; i < finder.n_dirs; ++i ) {
		printf("source dir: %s\n", finder.dirs[i]->path);
		path = LCFinder_CreateThumbDB( finder.dirs[i]->path );
		wprintf(L"thumbdb file: %s\n", path);
		finder.thumb_paths[i] = path;
	}
	printf("[thumbdb] init done\n");
}

/** 退出缩略图数据库 */
static void LCFinder_ExitThumbDB( void )
{
	int i;
	ThumbDB db;
	DictEntry *entry;
	DictIterator *iter;
	printf("[thumbdb] exit ..\n");
	iter = Dict_GetIterator( finder.thumb_dbs );
	entry = Dict_Next( iter );
	while( entry ) {
		db = DictEntry_GetVal( entry );
		ThumbDB_Close( db );
		entry = Dict_Next( iter );
	}
	Dict_ReleaseIterator( iter );
	Dict_Release( finder.thumb_dbs );
	for( i = 0; i < finder.n_dirs; ++i ) {
		free( finder.thumb_paths[i] );
		finder.thumb_paths[i] = NULL;
	}
	free( finder.thumb_paths );
	finder.thumb_paths = NULL;
	finder.thumb_dbs = NULL;
	printf("[thumbdb] exit done\n");
}

/** 清除缩略图数据库 */
void LCFinder_ClearThumbDB( void )
{
	int i;
	ThumbDB db;
	wchar_t *path;
	DictEntry *entry;
	DictIterator *iter;
	iter = Dict_GetIterator( finder.thumb_dbs );
	entry = Dict_Next( iter );
	while( entry ) {
		db = DictEntry_GetVal( entry );
		ThumbDB_Close( db );
		entry = Dict_Next( iter );
	}
	Dict_ReleaseIterator( iter );
	Dict_Release( finder.thumb_dbs );
	for( i = 0; i < finder.n_dirs; ++i ) {
		path = finder.thumb_paths[i];
		finder.thumb_paths[i] = NULL;
		_wremove( path );
		free( path );
	}
	free( finder.thumb_paths );
	finder.thumb_paths = NULL;
	finder.thumb_dbs = NULL;
	LCFinder_InitThumbDB();
	LCFinder_TriggerEvent( EVENT_THUMBDB_DEL_DONE, NULL );
}

static void LCFinder_Exit( LCUI_SysEvent e, void *arg )
{
	UI_Exit();
	LCFinder_ExitThumbDB();
}

int main( int argc, char **argv )
{
//#define DEBUG
#if defined (LCUI_BUILD_IN_WIN32) && defined (DEBUG)
	InitConsoleWindow();
#endif
	LCFinder_InitWorkDir();
	LCFInder_InitFileDB();
	LCFinder_InitThumbDB();
	finder.trigger = EventTrigger();
	UI_Init();
	LCUI_BindEvent( LCUI_QUIT, LCFinder_Exit, NULL, NULL );
	return UI_Run();
}
