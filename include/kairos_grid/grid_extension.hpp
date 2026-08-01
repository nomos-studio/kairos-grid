// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

#include <functional>
#include <memory>
#include <string>

namespace kairos_grid {

// Factory record for a single module type.  Either the plain make/setup pair
// (for modules with no constructor arguments) or the make_with_args/
// setup_with_args pair (for modules that receive a runtime path or key, such
// as WasmGridModule).
struct ModuleSpec {
    std::function<std::unique_ptr<GridModule>()>                             make;
    std::function<void(GridModule*, const std::string&)>                     setup;
    std::function<std::unique_ptr<GridModule>(const std::string&)>           make_with_args;
    std::function<void(GridModule*, const std::string&, const std::string&)> setup_with_args;
};

// Abstract registry passed to every .kgext entry point.
// Extensions call add() once per module type they provide.
//
// companion_path(ext) locates a co-located companion file. Neural extensions
// call r.companion_path(".kgnwt") to get the weights bundle path alongside
// their .kgext. Returns empty string when there is no companion or the
// context carries no path (e.g. unit tests, populate_builtins).
class GridModuleRegistry {
  public:
    virtual void        add(std::string key, ModuleSpec spec) = 0;
    virtual std::string companion_path(const std::string& /*ext*/) const { return {}; }
    virtual ~GridModuleRegistry() = default;
};

} // namespace kairos_grid

// Every .kgext shared library must export this symbol with C linkage.
// kairos-grid calls it after dlopen(), passing the live registry.
// The extension populates the registry with its module types.
//
// extern "C" avoids C++ name mangling so dlsym() can locate the symbol
// without knowing the compiler's mangling scheme.
#define KAIROS_GRID_EXTENSION_ENTRY "kairos_grid_extension_entry"
extern "C" {
typedef void(kairos_grid_extension_entry_fn)(kairos_grid::GridModuleRegistry&);
}
