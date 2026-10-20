// Harmless loader fixture with no XeLL exports; never included in the product.
extern "C" __declspec(dllexport) int NativeXeLLNegativeFixture() { return 1; }
