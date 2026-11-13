/*
 * Supremacy port - file and folder requesters on AmigaOS 4 (asl.library).
 *
 * The launcher uses SDL's file dialogs on other systems. On AmigaOS 4 the
 * SDL3 port's dialog runs in a thread of its own whose error buffer is not
 * set up: setting an error there reads through a null pointer (a DSI at
 * SDL_GetErrBuf from OS4_DialogThread, seen with SDL 3.4.18). So
 * the launcher opens the ASL requester itself, here, in its own thread.
 */
#include "os4_dialog.h"

#ifdef __amigaos4__
#include <string.h>

#include <dos/dos.h>
#include <libraries/asl.h>
#include <intuition/intuition.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>

void os4_message(const char *title, const char *text)
{
    struct Library *IntuitionBase = IExec->OpenLibrary("intuition.library", 50);
    if (!IntuitionBase) return;
    struct IntuitionIFace *II = (struct IntuitionIFace *)IExec->GetInterface(IntuitionBase, "main", 1, NULL);
    if (II) {
        struct EasyStruct es = {sizeof es, 0, (STRPTR)title, (STRPTR)"%s", (STRPTR)"OK", NULL, NULL};
        II->EasyRequest(NULL, &es, NULL, text);
        IExec->DropInterface((struct Interface *)II);
    }
    IExec->CloseLibrary(IntuitionBase);
}

int os4_request(const char *title, const char *initial, int folders, int many,
                const char *pattern, char *out, size_t n)
{
    out[0] = 0;
    struct Library *AslBase = IExec->OpenLibrary("asl.library", 50);
    if (!AslBase) return -1;
    struct AslIFace *IAsl = (struct AslIFace *)IExec->GetInterface(AslBase, "main", 1, NULL);
    if (!IAsl) { IExec->CloseLibrary(AslBase); return -1; }
    int r = 0;
    struct FileRequester *fr = IAsl->AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, title,
        ASLFR_InitialDrawer, initial ? initial : "",
        ASLFR_DrawersOnly, folders ? TRUE : FALSE,
        ASLFR_DoMultiSelect, many ? TRUE : FALSE,
        ASLFR_DoPatterns, pattern ? TRUE : FALSE,
        ASLFR_InitialPattern, pattern ? pattern : "#?",
        ASLFR_RejectIcons, TRUE,
        TAG_DONE);
    if (!fr) r = -1;
    else if (IAsl->AslRequest(fr, NULL)) {
        if (folders) {
            strncpy(out, fr->fr_Drawer, n - 1);
            out[n - 1] = 0;
            r = 1;
        } else if (many && fr->fr_NumArgs > 0) {
            for (LONG i = 0; i < fr->fr_NumArgs; i++) {
                char one[1024] = "";
                if (!IDOS->NameFromLock(fr->fr_ArgList[i].wa_Lock, one, sizeof one)) continue;
                IDOS->AddPart(one, fr->fr_ArgList[i].wa_Name, sizeof one);
                if (out[0] && strlen(out) + 1 < n) strcat(out, "\n");
                if (strlen(out) + strlen(one) < n) strcat(out, one);
            }
            r = out[0] ? 1 : 0;
        } else if (fr->fr_File && fr->fr_File[0]) {
            strncpy(out, fr->fr_Drawer, n - 1);
            out[n - 1] = 0;
            IDOS->AddPart(out, fr->fr_File, (ULONG)n);
            r = 1;
        }
    }
    if (fr) IAsl->FreeAslRequest(fr);
    IExec->DropInterface((struct Interface *)IAsl);
    IExec->CloseLibrary(AslBase);
    return r;
}
#endif
