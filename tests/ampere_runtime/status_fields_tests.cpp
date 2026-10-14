#include "ampere_status_fields.h"
#include "ui_status_json.h"
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    // The RTX 30 tester status from 1.4.2-dev.1 (Where Winds Meet, GA106) was
    // rejected whole by the menu because these two blocks were nested objects.
    ampere_provider_arch::Snapshot declaration{};
    declaration.armed = true; declaration.gpuArchitecture = 0x170; declaration.gpuImplementation = 6;
    declaration.declared = 2; declaration.last = ampere_provider_arch::Outcome::eDeclared;
    dlssg_provider_selection::Snapshot selection{};
    selection.prepared = true; selection.selected.version = {310, 9, 0}; selection.selected.nvidiaSigned = true;
    selection.attempts = 1; selection.loaded = 1; selection.examined = 3;
    const std::string path = "C:\\\\ProgramData\\\\NVIDIA\\\\NGX\\\\models\\\\dlssg\\\\versions\\\\20318464\\\\files\\\\160_E658700.bin";
    const std::string status = std::string("{\"version\":1,\"ampereFailure\":0")
        + ampere_status_fields::Declaration(declaration)
        + ampere_status_fields::ProviderSelection(selection, path) + "}";
    std::cout << status << '\n';
    Check(ui_status_json::CompleteObject(status), "RTX 30 status with both blocks is one complete flat object");
    Check(status.find("\"ampereDeclarationLast\":\"declared\"") != std::string::npos, "declaration outcome is published");
    Check(status.find("\"ampereProviderVersion\":\"310.9.0\"") != std::string::npos, "selected provider version is published");
    Check(status.find("\"ampereProviderPath\":\"C:\\\\ProgramData") != std::string::npos, "selected provider path is published escaped");
    Check(status.find(":{") == std::string::npos, "no nested object remains");
    // The 1.4.2-dev.1 layout, kept as a negative control for the reader.
    Check(!ui_status_json::CompleteObject("{\"version\":1,\"ampereArchitectureDeclaration\":{\"declared\":1}}"),
        "control: the reader rejects a nested object (the 1.4.2-dev.1 symptom)");
    const std::string empty = std::string("{\"version\":1")
        + ampere_status_fields::ProviderSelection(dlssg_provider_selection::Snapshot{}, "") + "}";
    Check(ui_status_json::CompleteObject(empty), "an empty selection stays a complete object");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
