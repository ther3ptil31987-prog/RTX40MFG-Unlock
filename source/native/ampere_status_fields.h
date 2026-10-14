#pragma once
#include "ampere_provider_arch.h"
#include "dlssg_provider_selection.h"
#include <cstdio>
#include <string>

// RTX 30 status fields. Each fragment starts with a comma and adds top-level
// keys only: the menu and control readers accept one flat object
// (ui_status_json::CompleteObject) and drop the whole snapshot otherwise.
namespace ampere_status_fields
{
inline std::string Declaration(const ampere_provider_arch::Snapshot& declaration)
{
    char fragment[448]{};
    std::snprintf(fragment, sizeof(fragment), ",\"ampereDeclarationGpuArchitecture\":%u,"
        "\"ampereDeclarationGpuImplementation\":%u,\"ampereDeclarationDeclared\":%llu,"
        "\"ampereDeclarationAlreadyDeclared\":%llu,\"ampereDeclarationShapeMismatches\":%llu,"
        "\"ampereDeclarationPublicationFailures\":%llu,\"ampereDeclarationLast\":\"%s\"",
        declaration.gpuArchitecture, declaration.gpuImplementation,
        static_cast<unsigned long long>(declaration.declared),
        static_cast<unsigned long long>(declaration.alreadyDeclared),
        static_cast<unsigned long long>(declaration.shapeMismatches),
        static_cast<unsigned long long>(declaration.publicationFailures),
        ampere_provider_arch::OutcomeName(declaration.last));
    return fragment;
}

// `escapedPath` is the selected provider path already escaped for a JSON string.
inline std::string ProviderSelection(const dlssg_provider_selection::Snapshot& selection,
    const std::string& escapedPath)
{
    char fragment[640]{};
    std::snprintf(fragment, sizeof(fragment), ",\"ampereProviderVersion\":\"%u.%u.%u\","
        "\"ampereProviderAttempts\":%llu,\"ampereProviderVerifiedLoads\":%llu,\"ampereProviderFailures\":%llu,"
        "\"ampereProviderScanLimited\":%s,\"ampereProviderNvidiaSigned\":%s,\"ampereProviderNativePinned\":%s,"
        "\"ampereProviderExamined\":%u,\"ampereProviderSignatureRejections\":%u,\"ampereProviderPath\":\"",
        selection.selected.version.major, selection.selected.version.minor, selection.selected.version.build,
        static_cast<unsigned long long>(selection.attempts), static_cast<unsigned long long>(selection.loaded),
        static_cast<unsigned long long>(selection.failures), selection.scanLimited ? "true" : "false",
        selection.selected.nvidiaSigned ? "true" : "false", selection.nativeProviderPinned ? "true" : "false",
        selection.examined, selection.signatureRejections);
    return std::string(fragment) + escapedPath + "\"";
}
}
