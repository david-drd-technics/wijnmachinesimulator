@echo off
setlocal
cd /d "%~dp0"
where javac >nul 2>nul
if errorlevel 1 (
  echo Java JDK 17 of nieuwer is vereist. javac is niet gevonden.
  pause
  exit /b 1
)
if not exist "lib" mkdir "lib"
if not exist "lib\jSerialComm-2.11.2.jar" (
  echo jSerialComm downloaden van Maven Central...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Invoke-WebRequest -UseBasicParsing 'https://repo.maven.apache.org/maven2/com/fazecast/jSerialComm/2.11.2/jSerialComm-2.11.2.jar' -OutFile 'lib\jSerialComm-2.11.2.jar' -ErrorAction Stop } catch { Write-Error $_; exit 1 }"
  if errorlevel 1 (
    echo Download mislukt. Controleer internetverbinding of plaats het jar-bestand handmatig in lib.
    pause
    exit /b 1
  )
)
if not exist "build" mkdir build
javac -encoding UTF-8 -cp "lib\jSerialComm-2.11.2.jar" -d build "src\be\technodream\wijnmachine\WijnmachineSimulator.java"
if errorlevel 1 (
  echo Java-compilatie mislukt.
  pause
  exit /b 1
)
java -cp "build;lib\jSerialComm-2.11.2.jar" be.technodream.wijnmachine.WijnmachineSimulator
if errorlevel 1 pause
endlocal
