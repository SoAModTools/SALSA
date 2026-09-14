#pragma once
#include "SalsaCore/Program/ProgramService.h"
#include "SpiceSCT/SctInstructionFactory.h"
namespace salsa::core::program {
// Detached typed values for selected-item editors. References are authored IDs;
// these helpers never build a physical document or allocate authored identity.
class ProgramValues final {
  public:
    static Result<ValueBuffer> compose(ValueKind, std::uint64_t, std::uint32_t, std::span<const ValueBuffer>);
    static Result<ValueBuffer> slice(const ValueBuffer &, ValueId);
    static Result<ValueBuffer> replace(const ValueBuffer &, ValueId root, ValueId target, const ValueBuffer &);
    static Result<spice::sct::SctCanonicalExpression> expression(const ValueBuffer &, ValueId);
    static Result<spice::sct::SctTextValue> text(const ValueBuffer &, ValueId);
    static Result<ValueBuffer> text(const spice::sct::SctTextValue &);
    // Factory defaults are a draft, with unresolved references still visible.
    static Result<ValueBuffer> instruction(const spice::sct::SctInstructionFactoryRequest &);
};
} // namespace salsa::core::program
