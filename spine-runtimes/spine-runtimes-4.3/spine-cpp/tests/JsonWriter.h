#ifndef Spine_JsonWriter_h
#define Spine_JsonWriter_h

#include <spine/SpineString.h>
#include <stdio.h>

namespace spine {

	class JsonWriter {
	private:
		String buffer;
		int depth;
		bool needsComma;

	public:
		JsonWriter() : depth(0), needsComma(false) {
		}

		void writeObjectStart() {
			writeCommaIfNeeded();
			buffer.append("{");
			depth++;
			needsComma = false;
		}

		void writeObjectEnd() {
			depth--;
			if (needsComma) {
				buffer.append("\n");
				writeIndent();
			}
			buffer.append("}");
			needsComma = true;
		}

		void writeArrayStart() {
			writeCommaIfNeeded();
			buffer.append("[");
			depth++;
			needsComma = false;
		}

		void writeArrayEnd() {
			depth--;
			if (needsComma) {
				buffer.append("\n");
				writeIndent();
			}
			buffer.append("]");
			needsComma = true;
		}

		void writeName(const char *name) {
			writeCommaIfNeeded();
			buffer.append("\n");
			writeIndent();
			buffer.append("\"");
			buffer.append(name);
			buffer.append("\": ");
			needsComma = false;
		}

		void writeValue(const String &value) {
			writeCommaIfNeeded();
			if (value.buffer() == nullptr) {
				buffer.append("null");
			} else {
				buffer.append("\"");
				buffer.append(escapeString(value));
				buffer.append("\"");
			}
			needsComma = true;
		}

		void writeValue(const char *value) {
			writeCommaIfNeeded();
			if (value == nullptr) {
				buffer.append("null");
			} else {
				buffer.append("\"");
				buffer.append(escapeString(String(value)));
				buffer.append("\"");
			}
			needsComma = true;
		}

		void writeValue(float value) {
			writeCommaIfNeeded();

			// Format float with 6 decimal places
			char temp[32];
			snprintf(temp, sizeof(temp), "%.6f", value);

			// Remove trailing zeros
			char *end = temp + strlen(temp) - 1;
			while (end > temp && *end == '0') {
				end--;
			}
			if (*end == '.') {
				end--;
			}
			*(end + 1) = '\0';

			buffer.append(temp);
			needsComma = true;
		}

		void writeValue(int value) {
			writeCommaIfNeeded();
			char temp[32];
			snprintf(temp, sizeof(temp), "%d", value);
			buffer.append(temp);
			needsComma = true;
		}

		void writeValue(bool value) {
			writeCommaIfNeeded();
			buffer.append(value ? "true" : "false");
			needsComma = true;
		}

		void writeValue(size_t value) {
			writeCommaIfNeeded();
			char temp[32];
			snprintf(temp, sizeof(temp), "%zu", value);
			buffer.append(temp);
			needsComma = true;
		}

		void writeValue(PropertyId value) {
			writeCommaIfNeeded();
			char temp[32];
			snprintf(temp, sizeof(temp), "%lld", (long long) value);
			buffer.append(temp);
			needsComma = true;
		}

		void writeNull() {
			writeCommaIfNeeded();
			buffer.append("null");
			needsComma = true;
		}

		void close() {
			buffer.append("\n");
		}

		String getString() const {
			return buffer;
		}

	private:
		void writeCommaIfNeeded() {
			if (needsComma) {
				buffer.append(",");
			}
		}

		void writeIndent() {
			for (int i = 0; i < depth; i++) {
				buffer.append("  ");
			}
		}

		String escapeString(const String &str) {
			String result("");
			const char *chars = str.buffer();
			if (chars) {
				for (size_t i = 0; i < str.length(); i++) {
					char c = chars[i];
					switch (c) {
						case '"':
							result.append("\\\"");
							break;
						case '\\':
							result.append("\\\\");
							break;
						case '\b':
							result.append("\\b");
							break;
						case '\f':
							result.append("\\f");
							break;
						case '\n':
							result.append("\\n");
							break;
						case '\r':
							result.append("\\r");
							break;
						case '\t':
							result.append("\\t");
							break;
						default:
							result.append(c);
							break;
					}
				}
			}
			return result;
		}
	};

}// namespace spine

#endif