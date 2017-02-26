#include "imagemodel.h"
#include "zlog.h"

#include "capstone/include/capstone.h"

ImageModel::ImageModel() : base(0){
    err = cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &handle);
    if(err != CS_ERR_OK){
        ELOG("failed to open capstone");
        return;
    }
    err = cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    if(err != CS_ERR_OK){
        ELOG("failed to set capstone option");
        return;
    }
}

ImageModel::~ImageModel(){
    cs_close(&handle);
}

void ImageModel::loadImage(const ZBinary &inbin, zu64 offset){
    base = offset;
    image = inbin;
}

zu64 ImageModel::addEntry(zu64 start_addr, ZString name){
    if(name.isEmpty())
        name = "loc_" + HEX(start_addr);
    return disassembleAddress(start_addr, { NAMED, name });
}

zu64 ImageModel::disassembleAddress(zu64 start_addr, Label label){
    // if this address is already disassembled we're done
    if(refs.contains(start_addr - base)){
        // set higher-priority tag
        if(label.type <= refs[start_addr - base].ltype){
            if(refs[start_addr - base].label != label.str)
                LOG("Rename " << refs[start_addr - base].label << " " << label.str);
            refs[start_addr - base].ltype = label.type;
            refs[start_addr - base].label = label.str;
        }
        return 0;
    }

    LOG("Disassemble from 0x" << HEX(start_addr));

    if(base > start_addr){
        ELOG("address in wrong range");
        return 0;
    }
    zu64 offset = start_addr - base;
    if(offset >= image.size()){
        ELOG("address out of bounds");
        return 0;
    }

    const zu8 *iptr = image.raw() + offset;
    zu64 isize = image.size() - offset;
    zu64 iaddr = base + offset;
    cs_insn *insn = cs_malloc(handle);
    zu64 total = 0;

    unsigned ldr_reg = ARM_REG_INVALID;
    zu64 ldr_addr = 0;
    zu64 ldr_data = 0;
    bool ldr_flag = false;
    bool stop = false;

    while(true){
        // disassemble instructions
        if(cs_disasm_iter(handle, &iptr, &isize, &iaddr, insn)){
            if(refs.contains(offset)){
                // ran into already disassembled code
                return total;
            }

            RefElem instr;
            instr.type = CODE;
            instr.size = insn->size;
            instr.str = ZString(insn->mnemonic) + " " + insn->op_str;
//            LOG("0x" << HEX(insn->address) << ": " << instr.str);

            instr.ctype = NORMAL;
            instr.ftype = F_STRING;

            instr.ltype = LNONE;
            if(base + offset == start_addr){
                instr.ltype = label.type;
                instr.label = label.str;
            }

            refs.add(offset, instr);

            RefElem *tins = &refs.get(offset);

            // Handle instruction
            switch(insn->id){
                // Jumps change control flow
                case ARM_INS_B: {
                    // Direct Branch
                    zassert(insn->detail->arm.op_count == 1 &&
                            insn->detail->arm.operands[0].type == ARM_OP_IMM);
                    zu64 jaddr = insn->detail->arm.operands[0].imm;

                    // set target
                    tins->ftype = F_TARGET;
                    tins->target = jaddr;
                    tins->str = ZString(insn->mnemonic) + " ";

                    ZString jname = "jump_" + HEX(jaddr);
                    LOG("jump " << jname);
                    total += disassembleAddress(jaddr, { JUMP, jname });

                    // Stop if unconditional
                    if(insn->detail->arm.cc == ARM_CC_AL){
                        stop = true;
                    }
                    break;
                }
                case ARM_INS_CBZ:
                case ARM_INS_CBNZ: {
                    // Conditional Branch
                    zu64 addr = insn->detail->arm.operands[1].imm;

                    tins->ftype = F_TARGET;
                    tins->target = addr;
                    tins->str = ZString(insn->mnemonic) + " " +
                            cs_reg_name(handle, insn->detail->arm.operands[0].reg) +
                            ", ";

                    ZString jname = "jump_" + HEX(addr);
                    LOG("jump " << jname);
                    total += disassembleAddress(addr, { JUMP, jname });
                    break;
                }
                case ARM_INS_BX:
                    // Branch register
                    if(ldr_reg != ARM_REG_INVALID && insn->detail->arm.operands[0].reg == ldr_reg){
                        // Indirect jump
                        zu64 addr = ldr_data & ~(zu64)1;

                        tins->ftype = F_TARGET;
                        tins->target = addr;
                        tins->str += " /* ";
                        tins->suffix = " */ ";

                        refs[ldr_addr].ftype = F_TARGET;
                        refs[ldr_addr].str = ".word ";
                        refs[ldr_addr].target = addr;

                        ZString jname = "jump_" + HEX(addr);
                        LOG("jmup " << jname);
                        total += disassembleAddress(addr, { JUMP, jname });

                        refs[base - addr].flags |= THUMBFUNC;

                    } else {
                        LOG("branch reg");
                    }
                    // unconditional
                    stop = true;
                    break;

                // Sometimes changes control flow
                case ARM_INS_POP:
                    // Pop stack
                    for(int i = 0; i < insn->detail->arm.op_count; ++i){
                        if(insn->detail->arm.operands[i].type == ARM_OP_REG &&
                                insn->detail->arm.operands[i].reg == ARM_REG_PC){
                            // PC popped
                            LOG("pop pc");
                            stop = true;
                        }
                    }
                    break;

                // Calls reference new functions
                case ARM_INS_BL: {
                    // Branch and link
                    zassert(insn->detail->arm.op_count == 1 &&
                            insn->detail->arm.operands[0].type == ARM_OP_IMM);

                    // Direct call
                    zu64 addr = insn->detail->arm.operands[0].imm;

                    tins->ftype = F_TARGET;
                    tins->target = addr;
                    tins->str = ZString(insn->mnemonic) + " ";

                    ZString cname = "call_" + HEX(addr);
                    LOG("call " << cname);
                    total += disassembleAddress(addr, { CALL, cname });
                    break;
                }
                case ARM_INS_BLX:
                    // Branch and link register
                    if(ldr_reg != ARM_REG_INVALID && insn->detail->arm.operands[0].reg == ldr_reg){
                        // Indirect call
                        zu64 addr = ldr_data - 1;

                        tins->ftype = F_TARGET;
                        tins->target = addr;
                        tins->str += " /* ";
                        tins->suffix = " */ ";

                        refs[ldr_addr].ftype = F_TARGET;
                        refs[ldr_addr].str = ".word ";
                        refs[ldr_addr].target = addr;

                        ZString cname = "call_" + HEX(addr);
                        LOG("call " << cname);
                        total += disassembleAddress(addr, { CALL, cname });

                        refs[base - addr].flags |= THUMBFUNC;
                    }
                    break;

                // Table branches
                case ARM_INS_TBB: {
                    // Table branch byte
                    if(insn->detail->arm.op_count == 1 &&
                            insn->detail->arm.operands[0].type == ARM_OP_MEM &&
                            insn->detail->arm.operands[0].mem.base == ARM_REG_PC){
                        // PC relative
                        zu64 min = ZU64_MAX;
                        for(zu64 i = 0; ; ++i){
                            // Keep track of soonest switch handler
                            if(base + offset + insn->size + i < min){
                                zu64 boff = base + offset + insn->size +
                                        (image[offset + insn->size + i] << 1);
                                // Check that offset is after the table so far
                                if(boff > base + offset + insn->size + i){
                                    min = boff;
                                    ZString bname = "switch_" + HEX(boff);
                                    LOG("switch " << bname);
                                    total += disassembleAddress(boff, { SWITCH, bname});
                                } else {
                                    break;
                                }
                            } else {
                                break;
                            }
                        }
                    }
                    // Instructions immediately after this are junk
                    return total;
                    break;
                }

                // Load from memory
                case ARM_INS_LDR: {
                    // Load
                    if(insn->detail->arm.op_count == 2 &&
                            insn->detail->arm.operands[1].type == ARM_OP_MEM &&
                            insn->detail->arm.operands[1].mem.base == ARM_REG_PC){
                        // PC-relative load
                        zu64 pc = (base + offset + 4) & ~(zu64)3;
                        zu64 laddr = pc + insn->detail->arm.operands[1].mem.disp;
                        image.seek(laddr - base);

                        // save for next loop, next insn->may use
                        ldr_addr = laddr - base;
                        ldr_reg = insn->detail->arm.operands[0].reg;
                        ldr_data = image.readleu32();
                        ldr_flag = true;

                        ZString dname = "data_" + HEX(laddr);
                        tins->ftype = F_TARGET;
                        tins->target = laddr;
                        tins->str += " /* ";
                        tins->suffix = " */ ";
//                        tins->str = ZString(insn->mnemonic) + " " +
//                                cs_reg_name(handle, insn->detail->arm.operands[0].reg) +
//                                ", =";

                        LOG("load " << dname << " (" << HEX(ldr_data) << ")");

                        // add data ref
                        RefElem data;
                        data.type = DATA;
                        data.size = 4;
                        data.str = ".word 0x" + HEX(ldr_data);
                        data.label = "data_" + HEX(laddr);
                        refs.add(ldr_addr, data);
                    }
                    break;
                }

                default:
                    break;
            }

            total++;
            offset += insn->size;

            if(stop)
                break;

            if(ldr_flag){
                ldr_flag = false;
            } else {
                ldr_reg = ARM_REG_INVALID;
            }
        } else {
            ELOG("disassemble error: 0x" << HEX(base+offset) <<
                 " " << cs_strerror(cs_errno(handle)));
            break;
        }
    }

    cs_free(insn, 1);

    return total;
}

ZBinary ImageModel::makeCode(){
    ZBinary asem;
    asem.write((const zbyte *)".syntax unified\n", 16);
    asem.write((const zbyte *)".cpu cortex-m3\n", 15);
    asem.write((const zbyte *)".text\n", 6);
    asem.write((const zbyte *)".thumb\n\n", 8);

    reftype prev = DATA;

    for(zu64 i = 0; i < image.size();){
        if(refs.contains(i)){
            RefElem ref = refs[i];

            if(prev != ref.type){
                asem.write((const zbyte *)"\n", 1);
            }

            if(ref.flags & THUMBFUNC){
                asem.write((const zbyte *)".thumb_func\n", 12);
            }

            // add label, if any
            if(!ref.label.isEmpty()){
//                asem.write((const zbyte *)".thumb_func\n", 12);
                asem.write(ref.label.bytes(), ref.label.size());
                asem.write((const zbyte *)":\n", 2);
            }

            ZString istr = ref.str;
            switch(ref.ftype){
                case F_STRING:
                    // Just a string
                    istr = ref.str;
                    break;

                case F_TARGET:
                    // Get the label name of the target
                    if(refs.contains(ref.target - base)){
                        ZString lstr = refs.get(ref.target - base).label;
                        if(lstr.isEmpty()){
                            ELOG("missing target label " <<
                                 HEX(base + i) << " " << HEX(ref.target));
                            return ZBinary();
                        }
                        istr = ref.str + lstr + ref.suffix;
                    } else {
                        ELOG("missing target " << HEX(ref.target));
                        return ZBinary();
                    }
                    break;

                default:
                    break;
            }

            asem.write((const zbyte *)"    ", 4);
            asem.write(istr.bytes(), istr.size());
            asem.write((const zbyte *)"\n", 1);

            prev = ref.type;
            i += ref.size;

        } else {
            if(prev == CODE || prev == DATA)
                asem.write((const zbyte *)"\n", 1);

            if(0){
                // read data word if word-aligned
                image.seek(i);
                ZString data = "0x" + HEX(image.readleu32());

                // add data word
                asem.write((const zbyte *)".word ", 6);
                asem.write(data.bytes(), data.size());
                asem.write((const zbyte *)"\n", 1);
                i += 4;
            } else {
                // add pad byte
                ZString data = "0x" + HEX(image[i]);
                asem.write((const zbyte *)".byte ", 6);
                asem.write(data.bytes(), data.size());
                asem.write((const zbyte *)"\n", 1);
                i += 1;
            }

            prev = RAW;
        }
    }
    return asem;
}