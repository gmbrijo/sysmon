This is a basic Windows desktop application that monitors system resources in real time and shows a notification when usage crosses a set limit.
  
It tracks:  
  
CPU usage  
Memory usage  
Disk activity  
Network usage  
  
Users can set threshold values, and the app will trigger a Windows notification if those limits are exceeded.  
  
Features  
  >Real-time system monitoring  
  >Custom threshold input (CPU, RAM, Disk, Network)  
  >Notification alerts when limits are crossed  
  >Simple UI with usage bars and graphs  
  >Runs in system tray 
  
Tech used  
  >C++  
  >Win32 API  
  >PDH (Performance Data Helper) for system metrics  
  >Windows system tray notifications  
  >GDI for basic UI rendering  
  
Project structure  
SysMonitor/  
├── SysMonitor.sln  
├── SysMonitor.vcxproj  
├── resource_monitor.h  
├── pdh_collector.cpp  
├── threshold_dialog.cpp  
├── app.cpp  
├── main.cpp  
  
How to build  
  >Open SysMonitor.sln in Visual Studio  
  >Select Release | x64  
  >Build using Ctrl + Shift + B  
  >Run the executable

Usage  
  >Start the application  
  >Set threshold values  
  >Let it run in the background  
  >You’ll get notified if usage goes above your limits  
