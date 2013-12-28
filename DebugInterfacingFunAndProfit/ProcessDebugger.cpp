#include "stdafx.h"
#include "QtExpect.h"
#include "IProcessDebugger.h"
#include "ProcessDebugger.h"

const QString ERR = "Error Code: %1";

bool ProcessDebugger::StartAndAttachToProgram()
{
    bool success = EnableDebugging();
    if( success )
    {
        SHELLEXECUTEINFOW shellExecuteInfo;
        shellExecuteInfo.cbSize       = sizeof(SHELLEXECUTEINFO);
        shellExecuteInfo.fMask        = SEE_MASK_NOCLOSEPROCESS;
        shellExecuteInfo.hwnd         = NULL;               
        shellExecuteInfo.lpVerb       = L"open";
        shellExecuteInfo.lpFile       = m_ProgramName.c_str();
        shellExecuteInfo.lpParameters = m_ProgramArgs.c_str();
        shellExecuteInfo.lpDirectory  = NULL;
        shellExecuteInfo.nShow        = SW_SHOW;
        shellExecuteInfo.hInstApp     = NULL;

        bool success = (int)ShellExecuteExW( &shellExecuteInfo );

        if( success )
        {
            // Wait until mbam is in a state where it's able to process user input, and then attach to the process! :D
            WaitForInputIdle( shellExecuteInfo.hProcess, 60000 );
            success = AttachToProcess( GetProcessId( shellExecuteInfo.hProcess ) );
        }
    }

    return success;
}

bool ProcessDebugger::EnableDebugging()
{
    bool success = false;
    HANDLE processToken = NULL;
    DWORD ec = 0;

    // Start by getting the process token
    bool tokenOK = OpenProcessToken( GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &processToken );
    Q_ASSERT_X( tokenOK, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );

    // then look up the current privilege value 
    if( tokenOK )
    {
        TOKEN_PRIVILEGES tokenPrivs; 
        tokenPrivs.PrivilegeCount = 1;
        bool privsFound = LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tokenPrivs.Privileges[0].Luid);
        Q_ASSERT_X( privsFound, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );

        // Next we adjust the token privileges so that debugging is enabled...
        if( privsFound )
        {
            tokenPrivs.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            success = AdjustTokenPrivileges( processToken, FALSE, &tokenPrivs, sizeof(tokenPrivs), NULL, NULL );
            Q_ASSERT_X( success, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
        }
    }

    // Voila, and we're done, we can close the handle to our process
    Q_EXPECT( CloseHandle(processToken) );

    return success;
}

bool ProcessDebugger::AttachToProcess(DWORD processID)
{
    bool success = DebugActiveProcess( processID );
    Q_ASSERT_X( success, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
    return success;
}

void ProcessDebugger::OnCreateProcessEvent(DWORD ProcessId)
{
    qDebug() << "Created Process ID: " << ProcessId;
}

void ProcessDebugger::OnExitProcessEvent(DWORD ProcessId)
{
    qDebug() << "Exit Process ID: " << ProcessId;
}

void ProcessDebugger::OnCreateThreadEvent(DWORD ThreadId)
{
    qDebug() << "Create Thread ID: " << ThreadId;
}

void ProcessDebugger::OnExitThreadEvent(DWORD ThreadId)
{
    qDebug() << "Exit Thread ID: " << ThreadId;
}

void ProcessDebugger::OnLoadModuleEvent(LPVOID ImageBase, HANDLE hFile)
{
    if( m_DebugProcessHandle == NULL )
    {
        return;
    }

    // Get the module name from the file handle specified and add it to our list of module names
    std::wstring moduleName;
    if( !GetFileNameFromHandle(hFile, moduleName) )
    {
        moduleName = L"";
    }
    m_ModuleNames[ImageBase] = moduleName;

    // Now we can populate the symbols from that module (which we'll need later when we try to access qmbam application)

    // Obtain the module size and the address range occupied by the module

    DWORD moduleSize = 0;
    if( !GetModuleSize( m_hProcess, ImageBase, moduleSize ) )
    {
        moduleSize = 0; // Just in case
        _ASSERTE( !_T("GetModuleSize() failed.") );
    }

    LPVOID ImageEnd = (BYTE*)ImageBase + moduleSize;


    // Report the event

    _tprintf( _T("MODULE LOAD: %08p-%08p %s\n"), ImageBase, ImageEnd, ImageName.c_str() );


    // Load symbols for the module

    if( ( hFile != NULL ) && ( hFile != INVALID_HANDLE_VALUE )  )
    {
        _tprintf( _T("  Loading symbols...\n") );

        if( !m_SymbolEngine.LoadModuleSymbols( hFile, ImageName, (DWORD64)ImageBase, moduleSize ) )
        {
            _tprintf( _T("  Symbols cannot be loaded (error code: %u)\n"), m_SymbolEngine.LastError() );
        }
        else
        {
            ShowSymbolInfo( ImageBase );
        }
    }
    else 
    {
        _tprintf( _T("  Symbols cannot be loaded (file handle is null).\n") );
    }
}

void ProcessDebugger::OnUnloadModuleEvent(LPVOID ImageBase)
{

}

void ProcessDebugger::OnExceptionEvent(DWORD ThreadId, const EXCEPTION_DEBUG_INFO& Info)
{

}

void ProcessDebugger::OnDebugStringEvent(DWORD ThreadId, const OUTPUT_DEBUG_STRING_INFO& Info)
{

}

void ProcessDebugger::OnTimeout()
{
    qDebug() << "DebugLoop - Timeout!";
}

bool ProcessDebugger::HandleDebugEvent(DEBUG_EVENT &debugEvent, bool &initialBreakpointTriggered)
{
    bool keepDebugging = true;
    DWORD ContinueStatus = DBG_CONTINUE;

    switch( debugEvent.dwDebugEventCode ) 
    {

    case CREATE_PROCESS_DEBUG_EVENT:
        // With this event, the debugger receives the following handles:
        //   CREATE_PROCESS_DEBUG_INFO.hProcess - debuggee process handle
        //   CREATE_PROCESS_DEBUG_INFO.hThread  - handle to the initial thread of the debuggee process
        //   CREATE_PROCESS_DEBUG_INFO.hFile    - handle to the executable file that was 
        //                                        used to create the debuggee process (.EXE file)
        // 
        // hProcess and hThread handles will be closed by the operating system 
        // when the debugger calls ContinueDebugEvent after receiving 
        // EXIT_PROCESS_DEBUG_EVENT for the given process
        // 
        // hFile handle should be closed by the debugger, when the handle 
        // is no longer needed
        //
        // 
        // Save the process handle
        m_DebugProcessHandle = debugEvent.u.CreateProcessInfo.hProcess;

        OnCreateProcessEvent( debugEvent.dwProcessId );
        OnCreateThreadEvent( debugEvent.dwThreadId );
        OnLoadModuleEvent( debugEvent.u.CreateProcessInfo.lpBaseOfImage, debugEvent.u.CreateProcessInfo.hFile );

        Q_EXPECT( CloseHandle(debugEvent.u.CreateProcessInfo.hFile) );

        break;

    case EXIT_PROCESS_DEBUG_EVENT:
        // Handle the event
        OnExitProcessEvent( debugEvent.dwProcessId );

        // Reset the process handle (it will be closed at the next call to ContinueDebugEvent)
        m_DebugProcessHandle = NULL;
        keepDebugging = false;
        break;

    case CREATE_THREAD_DEBUG_EVENT:
        OnCreateThreadEvent( debugEvent.dwThreadId );

        // With this event, the debugger receives the following handle:
        //   CREATE_THREAD_DEBUG_INFO.hThread  - handle to the thread that has been created
        // 
        // This handle will be closed by the operating system 
        // when the debugger calls ContinueDebugEvent after receiving 
        // EXIT_THREAD_DEBUG_EVENT for the given thread
        // 

        break;

    case EXIT_THREAD_DEBUG_EVENT:
        OnExitThreadEvent( debugEvent.dwThreadId );
        break;

    case LOAD_DLL_DEBUG_EVENT:
        OnLoadModuleEvent( debugEvent.u.LoadDll.lpBaseOfDll, debugEvent.u.LoadDll.hFile );

        // With this event, the debugger receives the following handle:
        //   LOAD_DLL_DEBUG_INFO.hFile    - handle to the DLL file 
        // 
        // This handle should be closed by the debugger, when the handle 
        // is no longer needed
        //

        Q_EXPECT( CloseHandle(debugEvent.u.LoadDll.hFile) );

        // Note: Closing the file handle here can lead to the following side effect:
        //   After the file has been closed, the handle value will be reused 
        //   by the operating system, and if the next "load dll" debug event 
        //   comes (for another DLL), it can contain the file handle with the same 
        //   value (but of course the handle now refers to that another DLL). 
        //   Don't be surprised!
        //

        break;

    case UNLOAD_DLL_DEBUG_EVENT:
        OnUnloadModuleEvent( debugEvent.u.UnloadDll.lpBaseOfDll );
        break;

    case OUTPUT_DEBUG_STRING_EVENT:
        OnDebugStringEvent( debugEvent.dwThreadId, debugEvent.u.DebugString );
        break;

    case RIP_EVENT:
        keepDebugging = false;
        break;

    case EXCEPTION_DEBUG_EVENT:
        OnExceptionEvent( debugEvent.dwThreadId, debugEvent.u.Exception );

        // By default, do not handle the exception 
        // (let the debuggee handle it if it wants to)

        ContinueStatus = DBG_EXCEPTION_NOT_HANDLED;

        // Now the special case - the initial breakpoint 

        DWORD ExceptionCode = debugEvent.u.Exception.ExceptionRecord.ExceptionCode;

        if( !initialBreakpointTriggered && ( ExceptionCode == EXCEPTION_BREAKPOINT ) )
        {
            // This is the initial breakpoint, which is used to notify the debugger 
            // that the debuggee has initialized 
            // 
            // The debugger should handle this exception
            // 
            ContinueStatus = DBG_CONTINUE;

            initialBreakpointTriggered = true;
        }
        break;
    }

    // Let the debuggee continue 
    if( !ContinueDebugEvent( debugEvent.dwProcessId, debugEvent.dwThreadId, ContinueStatus ) )
    {
        qDebug() << "ContinueDebugEvent() failed. Error:" << GetLastError;
        keepDebugging = false;
    }

    return keepDebugging;
}

bool ProcessDebugger::ExecuteDebugLoop(DWORD timeout)
{
    // Run the debug loop and handle the events 

    DEBUG_EVENT debugEvent;

    bool keepDebugging = true;

    bool initialBreakpointTriggered = false;

    while( keepDebugging == true ) 
    {
        if( WaitForDebugEvent( &debugEvent, timeout ) )
        {
            keepDebugging = HandleDebugEvent(debugEvent, initialBreakpointTriggered);
        }
        else
        {
            // Did WaitForDebugEvent because of a timeout?
            DWORD ErrCode = GetLastError();

            if( ErrCode == ERROR_SEM_TIMEOUT ) 
            {
                // Yes, report and continue
                OnTimeout();
            }
            else 
            {
                qDebug() << "WaitForDebugEvent() failed. Error: " << GetLastError();
                return false;
            }
        }
    }

    return true;
}

bool ProcessDebugger::GetFileNameFromHandle( HANDLE hFile, std::wstring& fileName )
{
    DWORD ErrCode = 0;

    // Cleanup the [out] parameter
    fileName = L"";
    if( ( hFile == NULL ) || ( hFile == INVALID_HANDLE_VALUE ) )
    {
        return false;
    }

    if( !FileSizeIsValid(hFile) )
    {
        return false;
    }

    // Get the file name, map the file into memory, and handle any errors we might encounter...
    HANDLE hMapFile     = NULL;
    PVOID pViewOfFile   = NULL;

    // Map the file into memory, return if we fail to do so...
    hMapFile = CreateFileMapping( hFile, NULL, PAGE_READONLY, 0, 1, NULL );
    if( hMapFile == NULL )
    {
        Q_ASSERT_X( false, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
        return false;
    }
    
    pViewOfFile = MapViewOfFile( hMapFile, FILE_MAP_READ, 0, 0, 1 );
    if( pViewOfFile == NULL ) 
    {
        Q_ASSERT_X( false, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
        return false;
    }

    // Obtain the file name and save it to our output string...
    const DWORD BUFFER_SIZE = (MAX_PATH + 1);
    WCHAR fileNameBuffer[BUFFER_SIZE] = {0};
    if( !GetMappedFileName( GetCurrentProcess(), pViewOfFile, fileNameBuffer, BUFFER_SIZE) )
    {
        Q_ASSERT_X( false, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
        return false;
    }
    fileName = fileNameBuffer;

    // The file name returned by GetMappedFileName won't contain a drive letter, but a device name
    // (logically of course b/c everything has to be a pain in windows), so we have to do some
    // mojojojo to get the proper drive letter for the device.
    ReplaceDeviceNameWithDriveLetter( fileName );

    // Clean up our mapping and handle information
    if( pViewOfFile != NULL )
    {
        Q_EXPECT( UnmapViewOfFile( pViewOfFile ) );
    }

    if( hMapFile != NULL ) 
    {
        Q_EXPECT( CloseHandle( hMapFile ) );
    }

    return true;

}

bool ProcessDebugger::FileSizeIsValid(HANDLE hFile)
{
    // Does the file have a non-zero size ? (b/c files with zero size can't be mapped)
    DWORD FileSizeHi = 0;
    DWORD FileSizeLo = 0;

    // Get the file size and handle any failures callin that method (we can just return
    // in that case since there won't be any file name info to obtain)
    FileSizeLo = GetFileSize( hFile, &FileSizeHi );
    if( (FileSizeLo == INVALID_FILE_SIZE) && (GetLastError() != NO_ERROR) ) 
    {
        Q_ASSERT_X( false, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
        return false;
    }
    else if( ( FileSizeLo == 0 ) && ( FileSizeHi == 0 ) ) 
    {
        return false;
    }
    
    return true;
}


void ProcessDebugger::ReplaceDeviceNameWithDriveLetter( std::wstring& fileName )
{
    DWORD ErrCode = 0;

    if( fileName.length() == 0 )
    {
        return;
    }

    // Get the list of drive letters available on the syzytem
    const DWORD BUFFER_SIZE = 512;
    WCHAR driveNames[BUFFER_SIZE + 1] = {0};
    DWORD driveStringLength = GetLogicalDriveStrings( BUFFER_SIZE, driveNames );
    if( ( driveStringLength == 0 ) || ( driveStringLength > BUFFER_SIZE) )
    {
        Q_ASSERT_X( false, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
        return;
    }

    // Walk through the list of characters in the drive name string. If one of the corresponds with 
    // a device name specified in the string replace it with the drive letter specified...
    WCHAR *driveLetter = driveNames;

    do
    {
        WCHAR drive[3] = L" :";
        _tcsncpy( drive, driveLetter, 2 );
        WCHAR device[BUFFER_SIZE+1] = {0};
        driveStringLength = QueryDosDevice( drive, device, BUFFER_SIZE );
        if( ( driveStringLength == 0 ) || ( driveStringLength >= BUFFER_SIZE ) )
        {
            Q_ASSERT_X( false, Q_FUNC_INFO, QString(ERR).arg(GetLastError()).toAscii() );
        }
        else
        {
            // Is the device name the same as in the file name? If so then we can replace the device name with the drive letter!
            size_t deviceNameLength = _tcslen( device );
            if( _tcsnicmp( fileName.c_str(), device, deviceNameLength ) == 0 )
            {
                // Yes, it is -> Substitute it into the file name
                WCHAR newFileName[BUFFER_SIZE+1] = {0};
                _stprintf( newFileName, L"%s%s", drive, fileName.c_str()+deviceNameLength );
                fileName = newFileName;
                // the new file name has been populated, we can return now..
                return;
            }
        }
        while( *driveLetter++ );

      // Repeat until we've iterated through all of the drive letters.
    } while( *driveLetter );

}