#!/usr/bin/env bash
# Windows dedicated server test: the server (M:\Satismod\DedicatedServer, driven through M:/Satismod/tools/ds.sh)
# loads RFDDTest, one GameCopy client joins and logs its view (ds_client.txt), and this script drives the remote
# player's equipment on the server with RFDD dev commands. Output: this script's stdout plus both game logs.
# Usage: ds_test.sh <scratchpad dir with launch_gamecopy.ps1> [linux]
#   linux: the Linux server in the EasyDDLinux WSL distro (port 7787) instead of the Windows one.
set -u
SP="$1"
TARGET="${2:-windows}"
DS=/m/Satismod/tools/ds.sh
SAVES="$(cygpath "$LOCALAPPDATA")/FactoryGame/Saved/SaveGames"
CLIENT_SAVES=$(ls -d "$SAVES"/7656* | head -1)
wsl_run() { MSYS_NO_PATHCONV=1 wsl.exe -d EasyDDLinux -u ubuntu -- bash -c "$1" 2>&1 | tr -d '\0'; }

cmd() { echo "> $*"; bash "$DS" cmd "$*"; }

if [ "$TARGET" = linux ]; then
  export DS_HOST=$(MSYS_NO_PATHCONV=1 wsl.exe -d EasyDDLinux -- hostname -I | tr -d '\0\r' | awk '{print $1}') DS_PORT=7787 DS_STATE_DIR=/m/Satismod/DedicatedServerLinuxUser
  CONNECT="$DS_HOST:7787"
  echo "== Linux server save folder before:"; wsl_run 'ls -la ~/.config/Epic/FactoryGame/Saved/SaveGames/server/'
  wsl_run "cp '/mnt/c${CLIENT_SAVES#/c}/RFDDTest.sav' ~/.config/Epic/FactoryGame/Saved/SaveGames/server/RFDDTest.sav"
  wsl_run 'bash /mnt/m/Satismod/tools/linux/start-linux-server.sh' | tail -2
else
  CONNECT="127.0.0.1:7777"
  SERVER_SAVES="$SAVES/server"
  echo "== server save folder before:"; ls -la "$SERVER_SAVES"
  cp "$CLIENT_SAVES/RFDDTest.sav" "$SERVER_SAVES/RFDDTest.sav"
  bash "$DS" start
fi
bash "$DS" wait-api >/dev/null || { echo "API did not come up"; exit 1; }
if ! bash "$DS" QueryServerState | grep -q serverGameState; then
  echo "stored token rejected; claiming"; bash "$DS" claim
fi
bash "$DS" LoadGame '{"SaveName":"RFDDTest","EnableAdvancedGameSettings":false}'; echo
bash "$DS" wait-game >/dev/null || { echo "game did not start"; exit 1; }
cmd "net.AllowEncryption 1"
cmd "RFDD.Status"

echo "== launching the client"
printf '%s' "RFDD.ScriptOnLoad M:/Satismod/sfmod-reloadfromdd/Mods/ReloadFromDD/Tools/tests/ds_client.txt,open $CONNECT" > "$SP/exec_dsclient.txt"
rm -f "$SP/exec_dsclient.txt.pid"
powershell.exe -NoProfile -Command "Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','$(cygpath -w "$SP")\\launch_gamecopy.ps1','-ExecCmdsFile','$(cygpath -w "$SP")\\exec_dsclient.txt','-OffscreenSeconds','1500'"

echo "== waiting for the player"
CLIENT_LOG=/m/Satismod/GameCopyUser/Saved/Logs/FactoryGame.log
for i in $(seq 1 180); do
  if bash "$DS" QueryServerState | grep -q '"numConnectedPlayers":[1-9]'; then break; fi
  # Give up early if the client was refused (e.g. mismatched mod versions); the reason is in its log.
  if [ "$i" -gt 6 ] && grep -aq "PendingConnectionFailure" "$CLIENT_LOG" 2>/dev/null; then
    echo "!! client was refused:"; grep -a "NMT_Failure" "$CLIENT_LOG" | head -1 | cut -c1-240
    bash "$DS" stop; sleep 10
    [ "$TARGET" = linux ] && wsl_run 'rm -f ~/.config/Epic/FactoryGame/Saved/SaveGames/server/RFDDTest.sav'
    [ "$TARGET" = windows ] && rm -f "$SERVER_SAVES/RFDDTest.sav"
    [ -f "$SP/exec_dsclient.txt.pid" ] && echo "client pids: $(cat "$SP/exec_dsclient.txt.pid")"
    exit 2
  fi
  sleep 5
done
bash "$DS" QueryServerState; echo
sleep 30   # let the pawn spawn and the client's script start

cmd "RFDD.DevPlayer 0"
cmd "RFDD.InvFirst 1"

echo "== RIFLE (server-driven reload for the remote player): expect spare 40, then loaded 15, depot 25"
cmd "RFDD.Equip BP_EquipmentDescriptorRifle arms"
cmd "RFDD.Inv Desc_CartridgeStandard clear"
cmd "RFDD.Depot Desc_CartridgeStandard -100000"
cmd "RFDD.Depot Desc_CartridgeStandard 40"
cmd "RFDD.Use empty"
cmd "RFDD.Use reload"
sleep 6
cmd "RFDD.Use weapon"
sleep 20   # client logs its view of the rifle

echo "== BERRIES: hand 2, depot 10; eat twice; expect hand 1, depot 9"
cmd "RFDD.Depot Desc_Berry -100000"
cmd "RFDD.Depot Desc_Berry 10"
cmd "RFDD.Equip Desc_Berry arms 2"
sleep 3
cmd "RFDD.Use eat"
cmd "RFDD.Use eat"

echo "== GAS MASK: depot 10; charge; expect depot 9"
cmd "RFDD.Inv Desc_Filter clear"
cmd "RFDD.Depot Desc_Filter -100000"
cmd "RFDD.Depot Desc_Filter 10"
cmd "RFDD.Equip BP_EquipmentDescriptorGasmask head"
sleep 3
cmd "RFDD.Use charge head"

echo "== JETPACK: drain, then expect a refuel from the depot"
cmd "RFDD.Equip BP_EquipmentDescriptorJetPack back"
sleep 3
cmd "RFDD.Use jetpack"
cmd "RFDD.Use drain"
sleep 8
cmd "RFDD.Use jetpack"
sleep 20   # client logs its view of the jetpack

echo "== SERVER SWITCH: Ammo off for 25 s (the client logs it), then back on"
cmd "RFDD.Equip BP_EquipmentDescriptorRifle arms"
cmd "RFDD.Set Ammo false"
sleep 25
cmd "RFDD.Set Ammo true"
sleep 5
cmd "RFDD.Calls"
cmd "RFDD.Status"

echo "== stopping the server"
bash "$DS" stop
sleep 20
if [ "$TARGET" = linux ]; then
  wsl_run 'rm -f ~/.config/Epic/FactoryGame/Saved/SaveGames/server/RFDDTest.sav; ls -la ~/.config/Epic/FactoryGame/Saved/SaveGames/server/'
  echo "(autosaves named after the session \"Less Balls Experiments (with skip)\" are removed by the caller)"
else
  echo "== server save folder after (anything beyond the original files is removed by the caller):"; ls -la "$SERVER_SAVES"
fi
# The client quits by itself; report it if it is still running.
[ -f "$SP/exec_dsclient.txt.pid" ] && echo "client pids: $(cat "$SP/exec_dsclient.txt.pid")"
