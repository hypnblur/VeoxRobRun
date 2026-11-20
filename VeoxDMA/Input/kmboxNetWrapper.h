#pragma once

// Lightweight wrapper - no winsock/Windows conflicts.
// Implementation in kmboxNet.cpp
void KmBoxNetMouseMove(int x, int y);
void KmBoxNetMouseMoveToCenter();
void KmBoxNetMouseLeft(bool down);
void KmBoxNetMouseRight(bool down);
void KmBoxNetMouseMiddle(bool down);
void KmBoxNetMouseSide1(bool down);
void KmBoxNetMouseSide2(bool down);
void KmBoxNetKeyboardKey(unsigned char hid, bool down);
bool KmBoxNetIsConnected();
bool KmBoxNetConsumeReconnectRequested();
void KmBoxNetDisconnect();
