#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p lib build
if [ ! -f lib/jSerialComm-2.11.2.jar ]; then
  curl -fL "https://repo.maven.apache.org/maven2/com/fazecast/jSerialComm/2.11.2/jSerialComm-2.11.2.jar" -o lib/jSerialComm-2.11.2.jar
fi
javac -encoding UTF-8 -cp "lib/jSerialComm-2.11.2.jar" -d build src/be/technodream/wijnmachine/WijnmachineSimulator.java
java -cp "build:lib/jSerialComm-2.11.2.jar" be.technodream.wijnmachine.WijnmachineSimulator
