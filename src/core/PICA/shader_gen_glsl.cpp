#include "PICA/shader_gen.hpp"
using namespace PICA;
using namespace PICA::ShaderGen;

static constexpr const char* uniformDefinition = R"(
	layout(std140) uniform FragmentUniforms {
		int alphaReference;
		float depthScale;
		float depthOffset;

		vec4 constantColors[6];
		vec4 tevBufferColor;
		vec4 clipCoords;
	};
)";

std::string FragmentGenerator::getVertexShader(const PICARegs& regs) {
	std::string ret = "";

	switch (api) {
		case API::GL: ret += "#version 410 core"; break;
		case API::GLES: ret += "#version 300 es"; break;
		default: break;
	}

	if (api == API::GLES) {
		ret += R"(
			#define USING_GLES 1

			precision mediump int;
			precision mediump float;
		)";
	}

	ret += uniformDefinition;

	ret += R"(
		layout(location = 0) in vec4 a_coords;
		layout(location = 1) in vec4 a_quaternion;
		layout(location = 2) in vec4 a_vertexColour;
		layout(location = 3) in vec2 a_texcoord0;
		layout(location = 4) in vec2 a_texcoord1;
		layout(location = 5) in float a_texcoord0_w;
		layout(location = 6) in vec3 a_view;
		layout(location = 7) in vec2 a_texcoord2;

		out vec3 v_normal;
		out vec3 v_tangent;
		out vec3 v_bitangent;
		out vec4 v_colour;
		out vec3 v_texcoord0;
		out vec2 v_texcoord1;
		out vec3 v_view;
		out vec2 v_texcoord2;

	#ifndef USING_GLES
		out float gl_ClipDistance[2];
	#endif

		vec4 abgr8888ToVec4(uint abgr) {
			const float scale = 1.0 / 255.0;
			return scale * vec4(float(abgr & 0xffu), float((abgr >> 8) & 0xffu), float((abgr >> 16) & 0xffu), float(abgr >> 24));
		}

		vec3 rotateVec3ByQuaternion(vec3 v, vec4 q) {
			vec3 u = q.xyz;
			float s = q.w;
			return 2.0 * dot(u, v) * u + (s * s - dot(u, u)) * v + 2.0 * s * cross(u, v);
		}

		void main() {
			gl_Position = a_coords;
			vec4 colourAbs = abs(a_vertexColour);
			v_colour = min(colourAbs, vec4(1.f));

			v_texcoord0 = vec3(a_texcoord0.x, 1.0 - a_texcoord0.y, a_texcoord0_w);
			v_texcoord1 = vec2(a_texcoord1.x, 1.0 - a_texcoord1.y);
			v_texcoord2 = vec2(a_texcoord2.x, 1.0 - a_texcoord2.y);
			v_view = a_view;

			v_normal = normalize(rotateVec3ByQuaternion(vec3(0.0, 0.0, 1.0), a_quaternion));
			v_tangent = normalize(rotateVec3ByQuaternion(vec3(1.0, 0.0, 0.0), a_quaternion));
			v_bitangent = normalize(rotateVec3ByQuaternion(vec3(0.0, 1.0, 0.0), a_quaternion));

		#ifndef USING_GLES
			gl_ClipDistance[0] = -a_coords.z;
			gl_ClipDistance[1] = dot(clipCoords, a_coords);
		#endif
		}
)";

	return ret;
}

std::string FragmentGenerator::generate(const PICARegs& regs) {
	std::string ret = "";

	switch (api) {
		case API::GL: ret += "#version 410 core"; break;
		case API::GLES: ret += "#version 300 es"; break;
		default: break;
	}

	bool unimplementedFlag = false;
	if (api == API::GLES) {
		ret += R"(
			#define USING_GLES 1

			precision mediump int;
			precision mediump float;
		)";
	}

	// Input and output attributes
	ret += R"(
		in vec3 v_tangent;
		in vec3 v_normal;
		in vec3 v_bitangent;
		in vec4 v_colour;
		in vec3 v_texcoord0;
		in vec2 v_texcoord1;
		in vec3 v_view;
		in vec2 v_texcoord2;

		out vec4 fragColor;
		uniform sampler2D u_tex0;
		uniform sampler2D u_tex1;
		uniform sampler2D u_tex2;
		// GLES doesn't support sampler1DArray, as such we'll have to change how we handle lighting later
#ifndef USING_GLES
		uniform sampler1DArray u_tex_lighting_lut;
#endif
	)";

	ret += uniformDefinition;

	// Emit main function for fragment shader
	// When not initialized, source 13 is set to vec4(0.0) and 15 is set to the vertex colour
	ret += R"(
		void main() {
			vec4 combinerOutput = v_colour;
			vec4 previousBuffer = vec4(0.0);
			vec4 tevNextPreviousBuffer = tevBufferColor;			
	)";

	ret += R"(
		vec3 colorOp1 = vec3(0.0);
		vec3 colorOp2 = vec3(0.0);
		vec3 colorOp3 = vec3(0.0);

		float alphaOp1 = 0.0;
		float alphaOp2 = 0.0;
		float alphaOp3 = 0.0;
	)";

	// Get original depth value by converting from [near, far] = [0, 1] to [-1, 1]
	// We do this by converting to [0, 2] first and subtracting 1 to go to [-1, 1]
	ret += R"(
		float z_over_w = gl_FragCoord.z * 2.0f - 1.0f;
		float depth = z_over_w * depthScale + depthOffset;
	)";

	if ((regs[InternalRegs::DepthmapEnable] & 1) == 0) {
		ret += "depth /= gl_FragCoord.w;\n";
	}

	ret += "gl_FragDepth = depth;\n";

	textureConfig = regs[InternalRegs::TexUnitCfg];
	for (int i = 0; i < 6; i++) {
		compileTEV(ret, i, regs);
	}

	applyAlphaTest(ret, regs);

	ret += "fragColor = combinerOutput;\n";
	ret += "}"; // End of main function
	ret += "\n\n\n\n\n\n\n";

	return ret;
}

void FragmentGenerator::compileTEV(std::string& shader, int stage, const PICARegs& regs) {
	// Base address for each TEV stage's configuration
	static constexpr std::array<u32, 6> ioBases = {
		InternalRegs::TexEnv0Source, InternalRegs::TexEnv1Source, InternalRegs::TexEnv2Source,
		InternalRegs::TexEnv3Source, InternalRegs::TexEnv4Source, InternalRegs::TexEnv5Source,
	};

	const u32 ioBase = ioBases[stage];
	TexEnvConfig tev(regs[ioBase], regs[ioBase + 1], regs[ioBase + 2], regs[ioBase + 3], regs[ioBase + 4]);

	if (!tev.isPassthroughStage()) {
		// Get color operands
		shader += "colorOp1 = ";
		getColorOperand(shader, tev.colorSource1, tev.colorOperand1, stage);

		shader += ";\ncolorOp2 = ";
		getColorOperand(shader, tev.colorSource2, tev.colorOperand2, stage);

		shader += ";\ncolorOp3 = ";
		getColorOperand(shader, tev.colorSource3, tev.colorOperand3, stage);

		shader += ";\nvec3 outputColor" + std::to_string(stage) + " = clamp(";
		getColorOperation(shader, tev.colorOp);
		shader += ", vec3(0.0), vec3(1.0));\n";

		if (tev.colorOp == TexEnvConfig::Operation::Dot3RGBA) {
			// Dot3 RGBA also writes to the alpha component so we don't need to do anything more
			shader += "float outputAlpha" + std::to_string(stage) + " = outputColor" + std::to_string(stage) + ".x;\n";
		} else {
			// Get alpha operands
			shader += "alphaOp1 = ";
			getAlphaOperand(shader, tev.alphaSource1, tev.alphaOperand1, stage);

			shader += ";\nalphaOp2 = ";
			getAlphaOperand(shader, tev.alphaSource2, tev.alphaOperand2, stage);

			shader += ";\nalphaOp3 = ";
			getAlphaOperand(shader, tev.alphaSource3, tev.alphaOperand3, stage);

			shader += ";\nfloat outputAlpha" + std::to_string(stage) + " = clamp(";
			getAlphaOperation(shader, tev.alphaOp);
			// Clamp the alpha value to [0.0, 1.0]
			shader += ", 0.0, 1.0);\n";
		}

		shader += "combinerOutput = vec4(clamp(outputColor" + std::to_string(stage) + " * " + std::to_string(tev.getColorScale()) +
				  ".0, vec3(0.0), vec3(1.0)), clamp(outputAlpha" + std::to_string(stage) + " * " + std::to_string(tev.getAlphaScale()) +
				  ".0, 0.0, 1.0));\n";
	}

	shader += "previousBuffer = tevNextPreviousBuffer;\n\n";

	// Update the "next previous buffer" if necessary
	const u32 textureEnvUpdateBuffer = regs[InternalRegs::TexEnvUpdateBuffer];
	if (stage < 4) {
		// Check whether to update rgb
		if ((textureEnvUpdateBuffer & (0x100 << stage))) {
			shader += "tevNextPreviousBuffer.rgb = combinerOutput.rgb;\n";
		}

		// And whether to update alpha
		if ((textureEnvUpdateBuffer & (0x1000u << stage))) {
			shader += "tevNextPreviousBuffer.a = combinerOutput.a;\n";
		}
	}
}

void FragmentGenerator::getColorOperand(std::string& shader, TexEnvConfig::Source source, TexEnvConfig::ColorOperand color, int index) {
	using OperandType = TexEnvConfig::ColorOperand;

	// For inverting operands, add the 1.0 - x subtraction
	if (color == OperandType::OneMinusSourceColor || color == OperandType::OneMinusSourceRed || color == OperandType::OneMinusSourceGreen ||
		color == OperandType::OneMinusSourceBlue || color == OperandType::OneMinusSourceAlpha) {
		shader += "vec3(1.0, 1.0, 1.0) - ";
	}

	switch (color) {
		case OperandType::SourceColor:
		case OperandType::OneMinusSourceColor:
			getSource(shader, source, index);
			shader += ".rgb";
			break;

		case OperandType::SourceRed:
		case OperandType::OneMinusSourceRed:
			getSource(shader, source, index);
			shader += ".rrr";
			break;

		case OperandType::SourceGreen:
		case OperandType::OneMinusSourceGreen:
			getSource(shader, source, index);
			shader += ".ggg";
			break;

		case OperandType::SourceBlue:
		case OperandType::OneMinusSourceBlue:
			getSource(shader, source, index);
			shader += ".bbb";
			break;

		case OperandType::SourceAlpha:
		case OperandType::OneMinusSourceAlpha:
			getSource(shader, source, index);
			shader += ".aaa";
			break;

		default:
			shader += "vec3(1.0, 1.0, 1.0)";
			Helpers::warn("FragmentGenerator: Invalid TEV color operand");
			break;
	}
}

void FragmentGenerator::getAlphaOperand(std::string& shader, TexEnvConfig::Source source, TexEnvConfig::AlphaOperand color, int index) {
	using OperandType = TexEnvConfig::AlphaOperand;

	// For inverting operands, add the 1.0 - x subtraction
	if (color == OperandType::OneMinusSourceRed || color == OperandType::OneMinusSourceGreen || color == OperandType::OneMinusSourceBlue ||
		color == OperandType::OneMinusSourceAlpha) {
		shader += "1.0 - ";
	}

	switch (color) {
		case OperandType::SourceRed:
		case OperandType::OneMinusSourceRed:
			getSource(shader, source, index);
			shader += ".r";
			break;

		case OperandType::SourceGreen:
		case OperandType::OneMinusSourceGreen:
			getSource(shader, source, index);
			shader += ".g";
			break;

		case OperandType::SourceBlue:
		case OperandType::OneMinusSourceBlue:
			getSource(shader, source, index);
			shader += ".b";
			break;

		case OperandType::SourceAlpha:
		case OperandType::OneMinusSourceAlpha:
			getSource(shader, source, index);
			shader += ".a";
			break;

		default:
			shader += "1.0";
			Helpers::warn("FragmentGenerator: Invalid TEV color operand");
			break;
	}
}

void FragmentGenerator::getSource(std::string& shader, TexEnvConfig::Source source, int index) {
	switch (source) {
		case TexEnvConfig::Source::PrimaryColor: shader += "v_colour"; break;
		case TexEnvConfig::Source::Texture0: shader += "texture(u_tex0, v_texcoord0.xy)"; break;
		case TexEnvConfig::Source::Texture1: shader += "texture(u_tex1, v_texcoord1)"; break;
		case TexEnvConfig::Source::Texture2: {
			// If bit 13 in texture config is set then we use the texcoords for texture 1, otherwise for texture 2
			if (Helpers::getBit<13>(textureConfig)) {
				shader += "texture(u_tex2, v_texcoord1)";
			} else {
				shader += "texture(u_tex2, v_texcoord2)";
			}
			break;
		}

		case TexEnvConfig::Source::Previous: shader += "combinerOutput"; break;
		case TexEnvConfig::Source::Constant: shader += "constantColors[" + std::to_string(index) + "]"; break;
		case TexEnvConfig::Source::PreviousBuffer: shader += "previousBuffer"; break;
		
		// Lighting
		case TexEnvConfig::Source::PrimaryFragmentColor:
		case TexEnvConfig::Source::SecondaryFragmentColor: shader += "vec4(1.0, 1.0, 1.0, 1.0)"; break;

		default:
			Helpers::warn("Unimplemented TEV source: %d", static_cast<int>(source));
			shader += "vec4(1.0, 1.0, 1.0, 1.0)";
			break;
	}
}

void FragmentGenerator::getColorOperation(std::string& shader, TexEnvConfig::Operation op) {
	switch (op) {
		case TexEnvConfig::Operation::Replace: shader += "colorOp1"; break;
		case TexEnvConfig::Operation::Add: shader += "colorOp1 + colorOp2"; break;
		case TexEnvConfig::Operation::AddSigned: shader += "colorOp1 + colorOp2 - vec3(0.5)"; break;
		case TexEnvConfig::Operation::Subtract: shader += "colorOp1 - colorOp2"; break;
		case TexEnvConfig::Operation::Modulate: shader += "colorOp1 * colorOp2"; break;
		case TexEnvConfig::Operation::Lerp: shader += "mix(colorOp2, colorOp1, colorOp3)"; break;

		case TexEnvConfig::Operation::AddMultiply: shader += "min(colorOp1 + colorOp2, vec3(1.0)) * colorOp3"; break;
		case TexEnvConfig::Operation::MultiplyAdd: shader += "fma(colorOp1, colorOp2, colorOp3)"; break;
		case TexEnvConfig::Operation::Dot3RGB:
		case TexEnvConfig::Operation::Dot3RGBA: shader += "vec3(4.0 * dot(colorOp1 - vec3(0.5), colorOp2 - vec3(0.5)))"; break;
		default:
			Helpers::warn("FragmentGenerator: Unimplemented color op");
			shader += "vec3(1.0)";
			break;
	}
}

void FragmentGenerator::getAlphaOperation(std::string& shader, TexEnvConfig::Operation op) {
	switch (op) {
		case TexEnvConfig::Operation::Replace: shader += "alphaOp1"; break;
		case TexEnvConfig::Operation::Add: shader += "alphaOp1 + alphaOp2"; break;
		case TexEnvConfig::Operation::AddSigned: shader += "alphaOp1 + alphaOp2 - 0.5"; break;
		case TexEnvConfig::Operation::Subtract: shader += "alphaOp1 - alphaOp2"; break;
		case TexEnvConfig::Operation::Modulate: shader += "alphaOp1 * alphaOp2"; break;
		case TexEnvConfig::Operation::Lerp: shader += "mix(alphaOp2, alphaOp1, alphaOp3)"; break;

		case TexEnvConfig::Operation::AddMultiply: shader += "min(alphaOp1 + alphaOp2, 1.0) * alphaOp3"; break;
		case TexEnvConfig::Operation::MultiplyAdd: shader += "fma(alphaOp1, alphaOp2, alphaOp3)"; break;
		default:
			Helpers::warn("FragmentGenerator: Unimplemented alpha op");
			shader += "1.0";
			break;
	}
}

void FragmentGenerator::applyAlphaTest(std::string& shader, const PICARegs& regs) {
	const u32 alphaConfig = regs[InternalRegs::AlphaTestConfig];
	const auto function = static_cast<CompareFunction>(Helpers::getBits<4, 3>(alphaConfig));

	// Alpha test disabled
	if (Helpers::getBit<0>(alphaConfig) == 0 || function == CompareFunction::Always) {
		return;
	}

	shader += "int testingAlpha = int(combinerOutput.a * 255.0);\n";
	shader += "if (";
	switch (function) {
		case CompareFunction::Never: shader += "true"; break;
		case CompareFunction::Always: shader += "false"; break;
		case CompareFunction::Equal: shader += "testingAlpha != alphaReference"; break;
		case CompareFunction::NotEqual: shader += "testingAlpha == alphaReference"; break;
		case CompareFunction::Less: shader += "testingAlpha >= alphaReference"; break;
		case CompareFunction::LessOrEqual: shader += "testingAlpha > alphaReference"; break;
		case CompareFunction::Greater: shader += "testingAlpha <= alphaReference"; break;
		case CompareFunction::GreaterOrEqual: shader += "testingAlpha < alphaReference"; break;

		default:
			Helpers::warn("Unimplemented alpha test function");
			shader += "false";
			break;
	}

	shader += ") { discard; }\n";
}
