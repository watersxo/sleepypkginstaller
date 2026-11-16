#include <orbis/libkernel.h>
#include <orbis/SystemService.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void Notify(const char* FMT, ...)
{
	OrbisNotificationRequest Buffer;

	memset(&Buffer, 0, sizeof(Buffer));

	va_list args;
	va_start(args, FMT);
	vsnprintf(Buffer.message, sizeof(Buffer.message), FMT, args);
	va_end(args);

	Buffer.type = NotificationRequest;
	Buffer.unk3 = 0;
	Buffer.useIconImageUri = 1;
	Buffer.targetId = -1;
	strlcpy(Buffer.iconUri, "cxml://psnotification/tex_icon_system", sizeof(Buffer.iconUri));

	sceKernelSendNotificationRequest(0, &Buffer, 3120, 0);
}

void KernelPrintOut(const char* FMT, ...)
{
	char MessageBuf[1024];
	va_list args;
	va_start(args, FMT);
	vsnprintf(MessageBuf, sizeof(MessageBuf), FMT, args);
	va_end(args);

	sceKernelDebugOutText(0, MessageBuf);
}

void SafeExit(const char* reason, ...)
{
	char MessageBuf[1024];
	va_list args;
	va_start(args, reason);
	vsnprintf(MessageBuf, sizeof(MessageBuf), reason, args);
	va_end(args);

	KernelPrintOut("%s", MessageBuf);
	sceSystemServiceLoadExec((char*)"exit", NULL);
}
