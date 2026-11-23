#pragma once
// Headless full-duplex 64x64 AoIP mode for the `sender` CLI (`--duplex`).

bool lv_cli_wants_duplex(int argc, char** argv);
int lv_cli_run_duplex(int argc, char** argv);
