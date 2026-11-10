#pragma once
class CheatFunction
{
	int MsSleep = 0;
	std::function<void()> Function;
	ULONGLONG LastExecution = 0;
public:
	CheatFunction(int time, std::function<void()>func);
	void Execute();
};