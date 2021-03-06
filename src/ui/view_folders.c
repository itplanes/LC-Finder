﻿/* ***************************************************************************
* view_home.c -- folders view
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
* view_home.c -- "文件夹" 视图
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

#include "ui.h"
#include "finder.h"
#include <stdio.h>
#include <string.h>
#include <LCUI/timer.h>
#include <LCUI/display.h>
#include <LCUI/gui/widget.h>
#include <LCUI/gui/widget/textview.h>
#include "thumbview.h"

#define THUMB_CACHE_SIZE (20*1024*1024)

/** 视图同步功能的相关数据 */
typedef struct ViewSyncRec_ {
	LCUI_Thread tid;
	LCUI_BOOL is_running;
	LCUI_Mutex mutex;
	LCUI_Cond ready;
	int prev_item_type;
} ViewSyncRec, *ViewSync;

/** 文件扫描功能的相关数据 */
typedef struct FileScannerRec_ {
	LCUI_Thread tid;
	LCUI_Cond cond;
	LCUI_Mutex mutex;
	LCUI_BOOL is_running;
	LinkedList files;
} FileScannerRec, *FileScanner;

typedef struct FileEntryRec_ {
	LCUI_BOOL is_dir;
	DB_File file;
	char *path;
} FileEntryRec, *FileEntry;

static struct FoldersViewData {
	DB_Dir dir;
	LCUI_Widget view;
	LCUI_Widget items;
	LCUI_Widget info;
	LCUI_Widget info_name;
	LCUI_Widget info_path;
	LCUI_Widget tip_empty;
	ViewSyncRec viewsync;
	FileScannerRec scanner;
	LinkedList files;
} this_view;

void OpenFolder( const char *dirpath );

static void OnAddDir( void *privdata, void *data )
{
	DB_Dir dir = data;
	_DEBUG_MSG("add dir: %s\n", dir->path);
	if( !this_view.dir ) {
		OpenFolder( NULL );
	}
}

static void OnDelDir( void *privdata, void *data )
{
	OpenFolder( NULL );
}

static void OnBtnSyncClick( LCUI_Widget w, LCUI_WidgetEvent e, void *arg )
{
	LCFinder_TriggerEvent( EVENT_SYNC, NULL );
}

static void OnBtnReturnClick( LCUI_Widget w, LCUI_WidgetEvent e, void *arg )
{
	OpenFolder( NULL );
}

static void OnDeleteFileEntry( void *arg )
{
	FileEntry entry = arg;
	if( entry->is_dir ) {
		free( entry->path );
	} else {
		free( entry->file->path );
		free( entry->file );
	}
	free( entry );
}

static int FileScanner_ScanDirs( FileScanner scanner, char *path )
{
	char *name;
	LCUI_Dir dir;
	wchar_t *wpath;
	FileEntry file_entry;
	LCUI_DirEntry *dir_entry;
	int count, len, dirpath_len;

	count = 0;
	len = strlen( path );
	dirpath_len = len;
	wpath = malloc( sizeof(wchar_t) * (len + 1) );
	LCUI_DecodeString( wpath, path, len + 1, ENCODING_UTF8 );
	LCUI_OpenDirW( wpath, &dir );
	while( (dir_entry = LCUI_ReadDir( &dir )) && scanner->is_running ) {
		wchar_t *wname = LCUI_GetFileNameW( dir_entry );
		/* 忽略 . 和 .. 文件夹 */
		if( wname[0] == L'.' ) {
			if( wname[1] == 0 || 
			    (wname[1] == L'.' && wname[2] == 0) ) {
				continue;
			}
		}
		if( !LCUI_FileIsDirectory( dir_entry ) ) {
			continue;
		}
		file_entry = NEW( FileEntryRec, 1 );
		file_entry->is_dir = TRUE;
		file_entry->file = NULL;
		len = LCUI_EncodeString( NULL, wname, 0, ENCODING_UTF8 ) + 1;
		name = malloc( sizeof(char) * len );
		LCUI_EncodeString( name, wname, len, ENCODING_UTF8 );
		len = len + dirpath_len;
		file_entry->path = malloc( sizeof( char ) * len );
		sprintf( file_entry->path, "%s%s", path, name );
		LCUIMutex_Lock( &scanner->mutex );
		LinkedList_Append( &scanner->files, file_entry );
		LCUICond_Signal( &scanner->cond );
		LCUIMutex_Unlock( &scanner->mutex );
		++count;
	}
	LCUI_CloseDir( &dir );
	return count;
}

static int FileScanner_ScanFiles( FileScanner scanner, char *path )
{
	DB_File file;
	DB_Query query;
	FileEntry entry;
	int i, total, count;
	DB_QueryTermsRec terms;
	terms.dirpath = path;
	terms.n_dirs = 0;
	terms.n_tags = 0;
	terms.limit = 50;
	terms.offset = 0;
	terms.score = NONE;
	terms.tags = NULL;
	terms.dirs = NULL;
	terms.create_time = NONE;
	query = DB_NewQuery( &terms );
	count = total = DBQuery_GetTotalFiles( query );
	while( scanner->is_running && count > 0 ) {
		DB_DeleteQuery( query );
		query = DB_NewQuery( &terms );
		i = count < terms.limit ? count : terms.limit;
		for( ; scanner->is_running && i > 0; --i ) {
			file = DBQuery_FetchFile( query );
			if( !file ) {
				break;
			}
			entry = NEW( FileEntryRec, 1 );
			entry->is_dir = FALSE;
			entry->file = file;
			entry->path = file->path;
			DEBUG_MSG("file: %s\n", file->path);
			LCUIMutex_Lock( &scanner->mutex );
			LinkedList_Append( &scanner->files, entry );
			LCUICond_Signal( &scanner->cond );
			LCUIMutex_Unlock( &scanner->mutex );
		}
		count -= terms.limit;
		terms.offset += terms.limit;
	}
	return total;
}

static int FileScanner_LoadSourceDirs( FileScanner scanner )
{
	char *path;
	int i, len, count = 0;
	for( i = 0; i < finder.n_dirs; ++i ) {
		FileEntry entry = NEW( FileEntryRec, 1 );
		if( !finder.dirs[i] ) {
			continue;
		}
		len = strlen( finder.dirs[i]->path ) + 1;
		path = malloc( sizeof( char ) * len );
		strcpy( path, finder.dirs[i]->path );
		entry->path = path;
		entry->is_dir = TRUE;
		LCUIMutex_Lock( &scanner->mutex );
		LinkedList_Append( &scanner->files, entry );
		LCUICond_Signal( &scanner->cond );
		DEBUG_MSG("dir: %s\n", entry->path);
		LCUIMutex_Unlock( &scanner->mutex );
		++count;
	}
	return count;
}

/** 初始化文件扫描 */
static void FileScanner_Init( FileScanner scanner )
{
	LCUICond_Init( &scanner->cond );
	LCUIMutex_Init( &scanner->mutex );
	LinkedList_Init( &scanner->files );
}

/** 重置文件扫描 */
static void FileScanner_Reset( FileScanner scanner )
{
	if( scanner->is_running ) {
		scanner->is_running = FALSE;
		LCUIThread_Join( scanner->tid, NULL );
	}
	LCUIMutex_Lock( &scanner->mutex );
	LinkedList_Clear( &scanner->files, OnDeleteFileEntry );
	LCUICond_Signal( &scanner->cond );
	LCUIMutex_Unlock( &scanner->mutex );
}

static void FileScanner_Destroy( FileScanner scanner )
{
	FileScanner_Reset( scanner );
	LCUICond_Destroy( &scanner->cond );
	LCUIMutex_Destroy( &scanner->mutex );
}

static void FileScanner_Thread( void *arg )
{
	int count;
	FileScanner scanner;
	scanner = &this_view.scanner;
	scanner->is_running = TRUE;
	if( arg ) {
		count = FileScanner_ScanDirs( scanner, arg );
		count += FileScanner_ScanFiles( scanner, arg );
		free( arg );
	} else {
		count = FileScanner_LoadSourceDirs( scanner );
	}
	DEBUG_MSG("scan files: %d\n", count);
	if( count > 0 ) {
		Widget_AddClass( this_view.tip_empty, "hide" );
		Widget_Hide( this_view.tip_empty );
	} else {
		Widget_RemoveClass( this_view.tip_empty, "hide" );
		Widget_Show( this_view.tip_empty );
	}
	scanner->is_running = FALSE;
	LCUIThread_Exit( NULL );
}

/** 开始扫描 */
static void FileScanner_Start( FileScanner scanner, char *path )
{
	LCUIThread_Create( &scanner->tid, FileScanner_Thread, path );
}

static void OnItemClick( LCUI_Widget w, LCUI_WidgetEvent e, void *arg )
{
	FileEntry entry = e->data;
	DEBUG_MSG( "open file: %s\n", path );
	if( entry->is_dir ) {
		OpenFolder( entry->path );
	} else {
		UIPictureView_Open( entry->path );
	}
}

/** 在缩略图列表准备完毕的时候 */
static void OnThumbViewReady( LCUI_Widget w, LCUI_WidgetEvent e, void *arg )
{
	LCUICond_Signal( &this_view.viewsync.ready );
}

/** 视图同步线程 */
static void ViewSync_Thread( void *arg )
{
	ViewSync vs;
	LCUI_Widget item;
	FileEntry entry;
	FileScanner scanner;
	LinkedListNode *node;
	vs = &this_view.viewsync;
	scanner = &this_view.scanner;
	LCUIMutex_Lock( &vs->mutex );
	/* 等待缩略图列表部件准备完毕 */
	while( this_view.items->state < WSTATE_READY ) {
		LCUICond_TimedWait( &vs->ready, &vs->mutex, 100 );
	}
	LCUIMutex_Unlock( &vs->mutex );
	vs->prev_item_type = -1;
	vs->is_running = TRUE;
	while( vs->is_running ) {
		LCUIMutex_Lock( &scanner->mutex );
		if( scanner->files.length == 0 ) {
			LCUICond_Wait( &scanner->cond, &scanner->mutex );
			if( !vs->is_running ) {
				LCUIMutex_Unlock( &scanner->mutex );
				break;
			}
		}
		LCUIMutex_Lock( &vs->mutex );
		node = LinkedList_GetNode( &scanner->files, 0 );
		if( !node ) {
			LCUIMutex_Unlock( &vs->mutex );
			LCUIMutex_Unlock( &scanner->mutex );
			continue;
		}
		entry = node->data;
		LinkedList_Unlink( &scanner->files, node );
		LCUIMutex_Unlock( &scanner->mutex );
		LinkedList_AppendNode( &this_view.files, node );
		if( vs->prev_item_type != -1 && 
		    vs->prev_item_type != entry->is_dir ) {
			LCUI_Widget separator = LCUIWidget_New(NULL);
			Widget_AddClass( separator, "divider" );
			ThumbView_Append( this_view.items, separator );
		}
		vs->prev_item_type = entry->is_dir;
		if( entry->is_dir ) {
			item = ThumbView_AppendFolder( 
				this_view.items, entry->path, 
				this_view.dir == NULL );
			DEBUG_MSG("append folder: %s\n", entry->path);
		} else {
			item = ThumbView_AppendPicture( 
				this_view.items, entry->path );
		}
		if( item ) {
			Widget_BindEvent( item, "click", OnItemClick, 
					  entry, NULL );
		}
		LCUIMutex_Unlock( &vs->mutex );
	}
	LCUIThread_Exit( NULL );
}

static void OpenFolder( const char *dirpath )
{
	int i, len;
	char *path = NULL;
	DB_Dir dir = NULL;
	if( dirpath ) {
		len = strlen( dirpath );
		path = malloc( sizeof( char )*(len + 2) );
		strcpy( path, dirpath );
		for( i = 0; i < finder.n_dirs; ++i ) {
			if( finder.dirs[i] && 
			    strstr( finder.dirs[i]->path, path ) ) {
				dir = finder.dirs[i];
				break;
			}
		}
		if( path[len - 1] != PATH_SEP ) {
			path[len++] = PATH_SEP;
			path[len] = 0;
		}
		TextView_SetText( this_view.info_name, getdirname( dirpath ) );
		TextView_SetText( this_view.info_path, dirpath );
		Widget_AddClass( this_view.view, "show-folder-info-box" );
	} else {
		Widget_RemoveClass( this_view.view, "show-folder-info-box" );
	}
	DEBUG_MSG("dirpath: %s\n", dirpath);
	DEBUG_MSG("reset file scanner\n");
	FileScanner_Reset( &this_view.scanner );
	LCUIMutex_Lock( &this_view.viewsync.mutex );
	ThumbView_Lock( this_view.items );
	DEBUG_MSG("clear thumb view items\n");
	ThumbView_Empty( this_view.items );
	this_view.dir = dir;
	this_view.viewsync.prev_item_type = -1;
	LinkedList_ClearData( &this_view.files, OnDeleteFileEntry );
	FileScanner_Start( &this_view.scanner, path );
	ThumbView_Unlock( this_view.items );
	LCUIMutex_Unlock( &this_view.viewsync.mutex );
	DEBUG_MSG("done\n");
}

static void OnSyncDone( void *privdata, void *arg )
{
	OpenFolder( NULL );
}

void UI_InitFoldersView( void )
{
	LCUI_Widget btn, btn_return, items;
	LinkedList_Init( &this_view.files );
	FileScanner_Init( &this_view.scanner );
	LCUICond_Init( &this_view.viewsync.ready );
	LCUIMutex_Init( &this_view.viewsync.mutex );
	items = LCUIWidget_GetById( "current-file-list" );
	btn = LCUIWidget_GetById( "btn-sync-folder-files" );
	btn_return = LCUIWidget_GetById( "btn-return-root-folder" );
	this_view.view = LCUIWidget_GetById( "view-folders");
	this_view.info = LCUIWidget_GetById( "view-folders-info-box" );
	this_view.info_name = LCUIWidget_GetById( "view-folders-info-box-name" );
	this_view.info_path = LCUIWidget_GetById( "view-folders-info-box-path" );
	this_view.tip_empty = LCUIWidget_GetById( "tip-empty-folder" );
	this_view.items = items;
	Widget_BindEvent( btn, "click", OnBtnSyncClick, NULL, NULL );
	Widget_BindEvent( items, "ready", OnThumbViewReady, NULL, NULL );
	Widget_BindEvent( btn_return, "click", OnBtnReturnClick, NULL, NULL );
	LCUIThread_Create( &this_view.viewsync.tid, ViewSync_Thread, NULL );
	LCFinder_BindEvent( EVENT_SYNC_DONE, OnSyncDone, NULL );
	LCFinder_BindEvent( EVENT_DIR_ADD, OnAddDir, NULL );
	LCFinder_BindEvent( EVENT_DIR_DEL, OnDelDir, NULL );
	OpenFolder( NULL );
}

void UI_ExitFolderView( void )
{
	this_view.viewsync.is_running = FALSE;
	FileScanner_Destroy( &this_view.scanner );
	LCUIThread_Join( this_view.viewsync.tid, NULL );
	LCUIMutex_Unlock( &this_view.viewsync.mutex );
}