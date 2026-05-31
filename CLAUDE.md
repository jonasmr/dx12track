* Project DX12 Resource tracking injection

This project is about making a tool that can be used to spawn a game, then injecting hooks into the D3D12 calls to watch all D3D12 Object allocations

- You should implement it using vs2022 and c/c++
- You're already in a vs2022 command prompt, so tools are available
- the project should consist of an .exe(console) file for control, and a .dll for injection into the game process


* Injection
	- Injection should be handled by the dll.
	- Injection should wrap the proper functions of DX12
	- Injection should work with all current versions of the agility SDKs


* Tracking

	- Track all objects created in D3D12.
		- Make sure the tracking works properly with D3D12's reference counting
		- Make sure you cover overloaded functions, like CreateCommittedResource, CreateCommittedResource1, CreateCommittedResource2
			- Reference the latest agility SDK for full reference of functions to hook
		- Make sure to also track object names
	- For objects that have memory usage also track that.
		- Group memory by kind: Heap, Committed and Placed.
	- Keep track of memory totals by Resource Type / Memory type


* Output

- Once running, print a nice table of all the objects live, and the memory totals, to the console
	- Make sure its extremely efficient (use WriteConsoleOutput)
- Log all resource Creation and destruction to a json file. 
	- Figure out a proper format


* Reference
	In the samples/ folder there is a checkout of DirectX-Graphics-Samples.
		Use those to test the project
		DirectX-Graphics-Samples\MiniEngine\Build\x64\Debug\Output\ModelViewer\ModelViewer.exe can be launched from DirectX-Graphics-Samples\MiniEngine\ModelViewer\
	In the agilitySDK/ folder there is a copy of the latest agility SDK

