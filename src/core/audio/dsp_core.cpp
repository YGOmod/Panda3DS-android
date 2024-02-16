#include "audio/dsp_core.hpp"

#include "audio/null_core.hpp"
#include "audio/teakra_core.hpp"

std::unique_ptr<Audio::DSPCore> Audio::makeDSPCore(DSPCore::Type type, Memory& mem) {
	std::unique_ptr<DSPCore> core;

	switch (type) {
		case DSPCore::Type::Null: core = std::make_unique<NullDSP>(mem); break;
		case DSPCore::Type::Teakra: core = std::make_unique<TeakraDSP>(mem); break;

		default:
			Helpers::warn("Invalid DSP core selected!");
			core = std::make_unique<NullDSP>(mem);
			break;
	}

	mem.setDSPMem(core->getDspMemory());
	return core;
}
