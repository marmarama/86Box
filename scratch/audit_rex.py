import re
import sys

def analyze_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    # Find functions of the form:
    # void
    # host_x86_*(...)
    # {
    # ...
    # }
    pattern = re.compile(r'^(?:void|static inline void)\s*\n(host_x86_[A-Za-z0-9_]+)\(([^)]*)\)\s*\{([^}]*)\}', re.MULTILINE | re.DOTALL)
    
    matches = pattern.findall(content)
    print(f"Analyzing {filepath}: found {len(matches)} functions.")

    reg_params = ['dst_reg', 'src_reg', 'src_reg_a', 'src_reg_b', 'dst_host_reg', 'src_host_reg', 'base_reg', 'idx_reg', 'index_reg']

    for name, params_str, body in matches:
        params = [p.strip() for p in params_str.split(',') if p.strip()]
        reg_args = []
        for p in params:
            tokens = p.split()
            if len(tokens) >= 2:
                pname = tokens[-1]
                if 'reg' in pname.lower():
                    reg_args.append(pname)

        if not reg_args:
            continue

        # Check if REX prefix or & 8 is mentioned
        has_rex = ('rex_byte' in body) or ('& 8' in body) or ('0x41' in body) or ('0x48' in body) or ('0x49' in body) or ('0x4c' in body)
        
        # Check if each reg arg is accounted for in REX / & 8 checks
        missing = []
        for r in reg_args:
            # If it's XMM only, it might not need REX if it's strictly XMM0..7
            # But let's check
            if r not in body:
                continue
            # If body has REX checks, does it mention r?
            if not has_rex:
                missing.append(r)
            else:
                # If there is a rex check, does it check this specific reg?
                if r not in body:
                    continue
                # Check if r appears in rex_byte or & 8 or if it's ignored
                if f"{r} & 8" not in body and f"rex_byte" in body:
                    # check if r is an argument to rex_byte
                    rex_calls = re.findall(r'rex_byte\([^)]*\)', body)
                    found_in_rex = any(r in call for call in rex_calls)
                    if not found_in_rex and f"{r} & 8" not in body:
                        missing.append(r)

        if missing or not has_rex:
            print(f"[{name}]")
            print(f"  Reg params: {reg_args}")
            print(f"  Has REX/&8: {has_rex}")
            print(f"  Potentially unhandled regs: {missing}")
            # print first few lines of body
            body_preview = '\n    '.join([line.strip() for line in body.strip().split('\n')[:4]])
            print(f"  Body preview:\n    {body_preview}\n")

if __name__ == '__main__':
    analyze_file('/home/marm/src/86Box/src/codegen_new/codegen_backend_x86-64_ops.c')
    analyze_file('/home/marm/src/86Box/src/codegen_new/codegen_backend_x86-64_ops_sse.c')
