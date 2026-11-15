#define MORE (*(unsigned short*)0xFE900)

#define NESTED (*((nestedType*)0xFE940))
#define NESTED_BYTE (*(u1*)0xFE943)
#define MIXED (*((Mixed*)0xFE950))
#define MIXED_RAW (*(u1*)0xFE952)
#define unsupported_index (((nestedType*)0xFE940)->arr[2])
