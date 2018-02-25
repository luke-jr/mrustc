//
//
//
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"

struct ProgramOptions
{
    ::std::string   infile;

    int parse(int argc, const char* argv[]);
};

Value MIRI_Invoke(ModuleTree& modtree, ::HIR::Path path, ::std::vector<Value> args);
Value MIRI_Invoke_Extern(const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args);
Value MIRI_Invoke_Intrinsic(const ::std::string& name, const ::HIR::PathParams& ty_params, ::std::vector<Value> args);

int main(int argc, const char* argv[])
{
    ProgramOptions  opts;

    if( opts.parse(argc, argv) )
    {
        return 1;
    }

    auto tree = ModuleTree {};

    tree.load_file(opts.infile);

    auto val_argc = Value( ::HIR::TypeRef{RawType::I32} );
    ::HIR::TypeRef  argv_ty { RawType::I8 };
    argv_ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Pointer, 0 });
    argv_ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Pointer, 0 });
    auto val_argv = Value(argv_ty);
    val_argc.write_bytes(0, "\0\0\0", 4);
    val_argv.write_bytes(0, "\0\0\0\0\0\0\0", argv_ty.get_size());

    ::std::vector<Value>    args;
    args.push_back(::std::move(val_argc));
    args.push_back(::std::move(val_argv));
    auto rv = MIRI_Invoke( tree, tree.find_lang_item("start"), ::std::move(args) );
    ::std::cout << rv << ::std::endl;

    return 0;
}

struct Ops {
    template<typename T>
    static int do_compare(T l, T r) {
        if( l == r ) {
            return 0;
        }
        else if( !(l != r) ) {
            // Special return value for NaN w/ NaN
            return 2;
        }
        else if( l < r ) {
            return -1;
        }
        else {
            return 1;
        }
    }
    static uint64_t do_unsigned(uint64_t l, uint64_t r, ::MIR::eBinOp op) {
        switch(op)
        {
        case ::MIR::eBinOp::ADD:    return l + r;
        case ::MIR::eBinOp::SUB:    return l - r;
        case ::MIR::eBinOp::MUL:    return l * r;
        case ::MIR::eBinOp::DIV:    return l / r;
        case ::MIR::eBinOp::MOD:    return l % r;
        default:
            LOG_BUG("Unexpected operation in Ops::do_unsigned");
        }
    }
};

Value MIRI_Invoke(ModuleTree& modtree, ::HIR::Path path, ::std::vector<Value> args)
{
    TRACE_FUNCTION_R(path, "");

    LOG_DEBUG(path);
    const auto& fcn = modtree.get_function(path);
    for(size_t i = 0; i < args.size(); i ++)
    {
        LOG_DEBUG("- Argument(" << i << ") = " << args[i]);
    }

    if( fcn.external.link_name != "" )
    {
        // External function!
        return MIRI_Invoke_Extern(fcn.external.link_name, fcn.external.link_abi, ::std::move(args));
    }

    struct State
    {
        ModuleTree& modtree;
        const Function& fcn;
        Value   ret;
        ::std::vector<Value>    args;
        ::std::vector<Value>    locals;
        ::std::vector<bool>     drop_flags;

        State(ModuleTree& modtree, const Function& fcn, ::std::vector<Value> args):
            modtree(modtree),
            fcn(fcn),
            ret(fcn.ret_ty),
            args(::std::move(args)),
            drop_flags(fcn.m_mir.drop_flags)
        {
            locals.reserve(fcn.m_mir.locals.size());
            for(const auto& ty : fcn.m_mir.locals)
            {
                if( ty == RawType::Unreachable ) {
                    // HACK: Locals can be !, but they can NEVER be accessed
                    locals.push_back(Value());
                }
                else {
                    locals.push_back(Value(ty));
                }
            }
        }

        ValueRef get_value_and_type(const ::MIR::LValue& lv, ::HIR::TypeRef& ty)
        {
            switch(lv.tag())
            {
            case ::MIR::LValue::TAGDEAD:    throw "";
            TU_ARM(lv, Return, _e) {
                ty = fcn.ret_ty;
                return ValueRef(ret, 0, ret.size());
                } break;
            TU_ARM(lv, Local, e) {
                ty = fcn.m_mir.locals.at(e);
                return ValueRef(locals.at(e), 0, locals.at(e).size());
                } break;
            TU_ARM(lv, Argument, e) {
                ty = fcn.args.at(e.idx);
                return ValueRef(args.at(e.idx), 0, args.at(e.idx).size());
                } break;
            TU_ARM(lv, Static, e) {
                // TODO: Type!
                return ValueRef(modtree.get_static(e), 0, modtree.get_static(e).size());
                } break;
            TU_ARM(lv, Index, e) {
                auto idx = get_value_ref(*e.idx).read_usize(0);
                ::HIR::TypeRef  array_ty;
                auto base_val = get_value_and_type(*e.val, array_ty);
                if( array_ty.wrappers.empty() )
                    throw "ERROR";
                if( array_ty.wrappers.front().type == TypeWrapper::Ty::Array )
                {
                    ty = array_ty.get_inner();
                    base_val.m_offset += ty.get_size() * idx;
                    return base_val;
                }
                else if( array_ty.wrappers.front().type == TypeWrapper::Ty::Slice )
                {
                    throw "TODO";
                }
                else
                {
                    throw "ERROR";
                }
                } break;
            TU_ARM(lv, Field, e) {
                ::HIR::TypeRef  composite_ty;
                auto base_val = get_value_and_type(*e.val, composite_ty);
                LOG_DEBUG("Field - " << composite_ty);
                size_t inner_ofs;
                ty = composite_ty.get_field(e.field_index, inner_ofs);
                base_val.m_offset += inner_ofs;
                return base_val;
                }
            TU_ARM(lv, Downcast, e) {
                ::HIR::TypeRef  composite_ty;
                auto base_val = get_value_and_type(*e.val, composite_ty);

                size_t inner_ofs;
                ty = composite_ty.get_field(e.variant_index, inner_ofs);
                LOG_TODO("Read from Downcast - " << lv);
                base_val.m_offset += inner_ofs;
                return base_val;
                }
            TU_ARM(lv, Deref, e) {
                ::HIR::TypeRef  ptr_ty;
                auto val = get_value_and_type(*e.val, ptr_ty);
                ty = ptr_ty.get_inner();
                // TODO: Slices and slice-like types have the same logic.
                if( ty == RawType::Str )
                {
                    LOG_ASSERT(val.m_size == 2*POINTER_SIZE, "Deref of a &str that isn't a fat-pointer sized value");
                    // There MUST be a relocation at this point with a valid allocation.
                    auto& val_alloc = val.m_alloc ? val.m_alloc : val.m_value->allocation;
                    LOG_ASSERT(val_alloc, "Deref of a value with no allocation (hence no relocations)");
                    LOG_TRACE("Deref " << val_alloc.alloc());
                    auto alloc = val_alloc.alloc().get_relocation(val.m_offset);
                    LOG_ASSERT(alloc, "Deref of a value with no relocation");
                    size_t ofs = val.read_usize(0);
                    size_t size = val.read_usize(POINTER_SIZE);
                    return ValueRef(::std::move(alloc), ofs, size);
                }
                // TODO: Trait objects and trait-object likes
                else
                {
                    LOG_ASSERT(val.m_size == POINTER_SIZE, "Deref of a value that isn't a pointer-sized value");
                    // There MUST be a relocation at this point with a valid allocation.
                    auto& val_alloc = val.m_alloc ? val.m_alloc : val.m_value->allocation;
                    LOG_ASSERT(val_alloc, "Deref of a value with no allocation (hence no relocations)");
                    LOG_TRACE("Deref " << val_alloc.alloc());
                    auto alloc = val_alloc.alloc().get_relocation(val.m_offset);
                    LOG_ASSERT(alloc, "Deref of a value with no relocation");
                    size_t ofs = val.read_usize(0);
                    return ValueRef(::std::move(alloc), ofs, ty.get_size());
                }
                } break;
            }
            throw "";
        }
        ValueRef get_value_ref(const ::MIR::LValue& lv)
        {
            ::HIR::TypeRef  tmp;
            return get_value_and_type(lv, tmp);
        }

        ::HIR::TypeRef get_lvalue_ty(const ::MIR::LValue& lv)
        {
            ::HIR::TypeRef  ty;
            get_value_and_type(lv, ty);
            return ty;
        }

        Value read_lvalue_with_ty(const ::MIR::LValue& lv, ::HIR::TypeRef& ty)
        {
            auto base_value = get_value_and_type(lv, ty);

            return base_value.read_value(0, ty.get_size());
        }
        Value read_lvalue(const ::MIR::LValue& lv)
        {
            ::HIR::TypeRef  ty;
            return read_lvalue_with_ty(lv, ty);
        }
        void write_lvalue(const ::MIR::LValue& lv, Value val)
        {
            //LOG_DEBUG(lv << " = " << val);
            ::HIR::TypeRef  ty;
            auto base_value = get_value_and_type(lv, ty);

            if(base_value.m_alloc) {
                base_value.m_alloc.alloc().write_value(base_value.m_offset, ::std::move(val));
            }
            else {
                base_value.m_value->write_value(base_value.m_offset, ::std::move(val));
            }
        }

        Value const_to_value(const ::MIR::Constant& c, ::HIR::TypeRef& ty)
        {
            switch(c.tag())
            {
            case ::MIR::Constant::TAGDEAD:  throw "";
            TU_ARM(c, Int, ce) {
                ty = ::HIR::TypeRef(ce.t);
                Value val = Value(ty);
                val.write_bytes(0, &ce.v, ::std::min(ty.get_size(), sizeof(ce.v)));  // TODO: Endian
                // TODO: If the write was clipped, sign-extend
                return val;
                } break;
            TU_ARM(c, Uint, ce) {
                ty = ::HIR::TypeRef(ce.t);
                Value val = Value(ty);
                val.write_bytes(0, &ce.v, ::std::min(ty.get_size(), sizeof(ce.v)));  // TODO: Endian
                return val;
                } break;
            TU_ARM(c, Bool, ce) {
                Value val = Value(::HIR::TypeRef { RawType::Bool });
                val.write_bytes(0, &ce.v, 1);
                return val;
                } break;
            TU_ARM(c, Float, ce) {
                ty = ::HIR::TypeRef(ce.t);
                Value val = Value(ty);
                if( ce.t.raw_type == RawType::F64 ) {
                    val.write_bytes(0, &ce.v, ::std::min(ty.get_size(), sizeof(ce.v)));  // TODO: Endian/format?
                }
                else if( ce.t.raw_type == RawType::F32 ) {
                    float v = static_cast<float>(ce.v);
                    val.write_bytes(0, &v, ::std::min(ty.get_size(), sizeof(v)));  // TODO: Endian/format?
                }
                else {
                    throw ::std::runtime_error("BUG: Invalid type in Constant::Float");
                }
                return val;
                } break;
            TU_ARM(c, Const, ce) {
                LOG_BUG("Constant::Const in mmir");
                } break;
            TU_ARM(c, Bytes, ce) {
                LOG_TODO("Constant::Bytes");
                } break;
            TU_ARM(c, StaticString, ce) {
                ty = ::HIR::TypeRef(RawType::Str);
                ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Borrow, 0 });
                Value val = Value(ty);
                val.write_usize(0, 0);
                val.write_usize(POINTER_SIZE, ce.size());
                val.allocation.alloc().relocations.push_back(Relocation { 0, AllocationPtr::new_string(&ce) });
                LOG_DEBUG(c << " = " << val);
                //return Value::new_dataptr(ce.data());
                return val;
                } break;
            TU_ARM(c, ItemAddr, ce) {
                // Create a value with a special backing allocation of zero size that references the specified item.
                if( const auto* fn = modtree.get_function_opt(ce) ) {
                    return Value::new_fnptr(ce);
                }
                LOG_TODO("Constant::ItemAddr - statics?");
                } break;
            }
            throw "";
        }
        Value const_to_value(const ::MIR::Constant& c)
        {
            ::HIR::TypeRef  ty;
            return const_to_value(c, ty);
        }
        Value param_to_value(const ::MIR::Param& p, ::HIR::TypeRef& ty)
        {
            switch(p.tag())
            {
            case ::MIR::Param::TAGDEAD: throw "";
            TU_ARM(p, Constant, pe)
                return const_to_value(pe, ty);
            TU_ARM(p, LValue, pe)
                return read_lvalue_with_ty(pe, ty);
            }
            throw "";
        }
        Value param_to_value(const ::MIR::Param& p)
        {
            ::HIR::TypeRef  ty;
            return param_to_value(p, ty);
        }

        ValueRef get_value_ref_param(const ::MIR::Param& p, Value& tmp, ::HIR::TypeRef& ty)
        {
            switch(p.tag())
            {
            case ::MIR::Param::TAGDEAD: throw "";
            TU_ARM(p, Constant, pe)
                tmp = const_to_value(pe, ty);
                return ValueRef(tmp, 0, ty.get_size());
            TU_ARM(p, LValue, pe)
                return get_value_and_type(pe, ty);
            }
            throw "";
        }
    } state { modtree, fcn, ::std::move(args) };

    size_t bb_idx = 0;
    for(;;)
    {
        const auto& bb = fcn.m_mir.blocks.at(bb_idx);

        for(const auto& stmt : bb.statements)
        {
            LOG_DEBUG("BB" << bb_idx << "/" << (&stmt - bb.statements.data()) << ": " << stmt);
            switch(stmt.tag())
            {
            case ::MIR::Statement::TAGDEAD: throw "";
            TU_ARM(stmt, Assign, se) {
                Value   new_val;
                switch(se.src.tag())
                {
                case ::MIR::RValue::TAGDEAD: throw "";
                TU_ARM(se.src, Use, re) {
                    new_val = state.read_lvalue(re);
                    } break;
                TU_ARM(se.src, Constant, re) {
                    new_val = state.const_to_value(re);
                    } break;
                TU_ARM(se.src, Borrow, re) {
                    ::HIR::TypeRef  src_ty;
                    ValueRef src_base_value = state.get_value_and_type(re.val, src_ty);
                    auto alloc = src_base_value.m_alloc;
                    if( !alloc )
                    {
                        if( !src_base_value.m_value->allocation )
                        {
                            src_base_value.m_value->create_allocation();
                        }
                        alloc = AllocationPtr(src_base_value.m_value->allocation);
                    }
                    if( alloc.is_alloc() )
                        LOG_DEBUG("- alloc=" << alloc << " (" << alloc.alloc() << ")");
                    else
                        LOG_DEBUG("- alloc=" << alloc);
                    size_t ofs = src_base_value.m_offset;
                    bool is_slice_like = src_ty.has_slice_meta();
                    src_ty.wrappers.insert(src_ty.wrappers.begin(), TypeWrapper { TypeWrapper::Ty::Borrow, static_cast<size_t>(re.type) });

                    new_val = Value(src_ty);
                    // ^ Pointer value
                    new_val.write_usize(0, ofs);
                    if( is_slice_like )
                    {
                        new_val.write_usize(POINTER_SIZE, src_base_value.m_size);
                    }
                    // - Add the relocation after writing the value (writing clears the relocations)
                    new_val.allocation.alloc().relocations.push_back(Relocation { 0, ::std::move(alloc) });
                    } break;
                TU_ARM(se.src, Cast, re) {
                    // Determine the type of cast, is it a reinterpret or is it a value transform?
                    // - Float <-> integer is a transform, anything else should be a reinterpret.
                    ::HIR::TypeRef  src_ty;
                    auto src_value = state.get_value_and_type(re.val, src_ty);

                    new_val = Value(re.type);
                    if( re.type == src_ty )
                    {
                        // No-op cast
                        new_val = src_value.read_value(0, re.type.get_size());
                    }
                    else if( !re.type.wrappers.empty() )
                    {
                        // Destination can only be a raw pointer
                        if( re.type.wrappers.at(0).type != TypeWrapper::Ty::Pointer ) {
                            throw "ERROR";
                        }
                        if( !src_ty.wrappers.empty() )
                        {
                            // Source can be either
                            if( src_ty.wrappers.at(0).type != TypeWrapper::Ty::Pointer
                                && src_ty.wrappers.at(0).type != TypeWrapper::Ty::Borrow ) {
                                throw "ERROR";
                            }

                            if( src_ty.get_size() > re.type.get_size() ) {
                                // TODO: How to casting fat to thin?
                                //LOG_TODO("Handle casting fat to thin, " << src_ty << " -> " << re.type);
                                new_val = src_value.read_value(0, re.type.get_size());
                            }
                            else 
                            {
                                new_val = src_value.read_value(0, re.type.get_size());
                            }
                        }
                        else
                        {
                            if( src_ty == RawType::Function )
                            {
                            }
                            else if( src_ty == RawType::USize )
                            {
                            }
                            else
                            {
                                ::std::cerr << "ERROR: Trying to pointer (" << re.type <<" ) from invalid type (" << src_ty << ")\n";
                                throw "ERROR";
                            }
                            new_val = src_value.read_value(0, re.type.get_size());
                        }
                    }
                    else if( !src_ty.wrappers.empty() )
                    {
                        // TODO: top wrapper MUST be a pointer
                        if( src_ty.wrappers.at(0).type != TypeWrapper::Ty::Pointer
                            && src_ty.wrappers.at(0).type != TypeWrapper::Ty::Borrow ) {
                            throw "ERROR";
                        }
                        // TODO: MUST be a thin pointer?

                        // TODO: MUST be an integer (usize only?)
                        if( re.type != RawType::USize ) {
                            LOG_ERROR("Casting from a pointer to non-usize - " << re.type << " to " << src_ty);
                            throw "ERROR";
                        }
                        new_val = src_value.read_value(0, re.type.get_size());
                    }
                    else
                    {
                        // TODO: What happens if there'a cast of something with a relocation?
                        switch(re.type.inner_type)
                        {
                        case RawType::Unreachable:  throw "BUG";
                        case RawType::Composite:    throw "ERROR";
                        case RawType::TraitObject:    throw "ERROR";
                        case RawType::Function:    throw "ERROR";
                        case RawType::Str:    throw "ERROR";
                        case RawType::Unit:   throw "ERROR";
                        case RawType::F32: {
                            float dst_val = 0.0;
                            // Can be an integer, or F64 (pointer is impossible atm)
                            switch(src_ty.inner_type)
                            {
                            case RawType::Unreachable:  throw "BUG";
                            case RawType::Composite:    throw "ERROR";
                            case RawType::TraitObject:  throw "ERROR";
                            case RawType::Function:     throw "ERROR";
                            case RawType::Char: throw "ERROR";
                            case RawType::Str:  throw "ERROR";
                            case RawType::Unit: throw "ERROR";
                            case RawType::Bool: throw "ERROR";
                            case RawType::F32:  throw "BUG";
                            case RawType::F64:  dst_val = static_cast<float>( src_value.read_f64(0) ); break;
                            case RawType::USize:    throw "TODO";// /*dst_val = src_value.read_usize();*/   break;
                            case RawType::ISize:    throw "TODO";// /*dst_val = src_value.read_isize();*/   break;
                            case RawType::U8:   dst_val = static_cast<float>( src_value.read_u8 (0) );  break;
                            case RawType::I8:   dst_val = static_cast<float>( src_value.read_i8 (0) );  break;
                            case RawType::U16:  dst_val = static_cast<float>( src_value.read_u16(0) );  break;
                            case RawType::I16:  dst_val = static_cast<float>( src_value.read_i16(0) );  break;
                            case RawType::U32:  dst_val = static_cast<float>( src_value.read_u32(0) );  break;
                            case RawType::I32:  dst_val = static_cast<float>( src_value.read_i32(0) );  break;
                            case RawType::U64:  dst_val = static_cast<float>( src_value.read_u64(0) );  break;
                            case RawType::I64:  dst_val = static_cast<float>( src_value.read_i64(0) );  break;
                            case RawType::U128: throw "TODO";// /*dst_val = src_value.read_u128();*/ break;
                            case RawType::I128: throw "TODO";// /*dst_val = src_value.read_i128();*/ break;
                            }
                            new_val.write_f32(0, dst_val);
                            } break;
                        case RawType::F64: {
                            double dst_val = 0.0;
                            // Can be an integer, or F32 (pointer is impossible atm)
                            switch(src_ty.inner_type)
                            {
                            case RawType::Unreachable:  throw "BUG";
                            case RawType::Composite:    throw "ERROR";
                            case RawType::TraitObject:  throw "ERROR";
                            case RawType::Function:     throw "ERROR";
                            case RawType::Char: throw "ERROR";
                            case RawType::Str:  throw "ERROR";
                            case RawType::Unit: throw "ERROR";
                            case RawType::Bool: throw "ERROR";
                            case RawType::F64:  throw "BUG";
                            case RawType::F32:  dst_val = static_cast<double>( src_value.read_f32(0) ); break;
                            case RawType::USize:    dst_val = static_cast<double>( src_value.read_usize(0) );   break;
                            case RawType::ISize:    dst_val = static_cast<double>( src_value.read_isize(0) );   break;
                            case RawType::U8:   dst_val = static_cast<double>( src_value.read_u8 (0) );  break;
                            case RawType::I8:   dst_val = static_cast<double>( src_value.read_i8 (0) );  break;
                            case RawType::U16:  dst_val = static_cast<double>( src_value.read_u16(0) );  break;
                            case RawType::I16:  dst_val = static_cast<double>( src_value.read_i16(0) );  break;
                            case RawType::U32:  dst_val = static_cast<double>( src_value.read_u32(0) );  break;
                            case RawType::I32:  dst_val = static_cast<double>( src_value.read_i32(0) );  break;
                            case RawType::U64:  dst_val = static_cast<double>( src_value.read_u64(0) );  break;
                            case RawType::I64:  dst_val = static_cast<double>( src_value.read_i64(0) );  break;
                            case RawType::U128: throw "TODO"; /*dst_val = src_value.read_u128();*/ break;
                            case RawType::I128: throw "TODO"; /*dst_val = src_value.read_i128();*/ break;
                            }
                            new_val.write_f64(0, dst_val);
                            } break;
                        case RawType::Bool:
                            LOG_TODO("Cast to " << re.type);
                        case RawType::Char:
                            LOG_TODO("Cast to " << re.type);
                        case RawType::USize:
                        case RawType::U8:
                        case RawType::U16:
                        case RawType::U32:
                        case RawType::U64:
                        case RawType::ISize:
                        case RawType::I8:
                        case RawType::I16:
                        case RawType::I32:
                        case RawType::I64:
                            {
                            uint64_t dst_val = 0.0;
                            // Can be an integer, or F32 (pointer is impossible atm)
                            switch(src_ty.inner_type)
                            {
                            case RawType::Unreachable:  throw "BUG";
                            case RawType::Composite:    throw "ERROR";
                            case RawType::TraitObject:  throw "ERROR";
                            case RawType::Function:
                                LOG_ASSERT(re.type.inner_type == RawType::USize, "");
                                new_val = src_value.read_value(0, re.type.get_size());
                                break;
                            case RawType::Char: LOG_TODO("Cast char to integer (only u32)");
                            case RawType::Str:  throw "ERROR";
                            case RawType::Unit: throw "ERROR";
                            case RawType::Bool: throw "ERROR";
                            case RawType::F64:  throw "BUG";
                            case RawType::F32:
                                dst_val = static_cast<uint64_t>( src_value.read_f32(0) );
                                if(0)
                            case RawType::USize:
                                dst_val = static_cast<uint64_t>( src_value.read_usize(0) );
                                if(0)
                            case RawType::ISize:
                                dst_val = static_cast<uint64_t>( src_value.read_isize(0) );
                                if(0)
                            case RawType::U8:
                                dst_val = static_cast<uint64_t>( src_value.read_u8 (0) );
                                if(0)
                            case RawType::I8:
                                dst_val = static_cast<uint64_t>( src_value.read_i8 (0) );
                                if(0)
                            case RawType::U16:
                                dst_val = static_cast<uint64_t>( src_value.read_u16(0) );
                                if(0)
                            case RawType::I16:
                                dst_val = static_cast<uint64_t>( src_value.read_i16(0) );
                                if(0)
                            case RawType::U32:
                                dst_val = static_cast<uint64_t>( src_value.read_u32(0) );
                                if(0)
                            case RawType::I32:
                                dst_val = static_cast<uint64_t>( src_value.read_i32(0) );
                                if(0)
                            case RawType::U64:
                                dst_val = static_cast<uint64_t>( src_value.read_u64(0) );
                                if(0)
                            case RawType::I64:
                                dst_val = static_cast<uint64_t>( src_value.read_i64(0) );

                                switch(re.type.inner_type)
                                {
                                case RawType::USize:
                                    new_val.write_usize(0, dst_val);
                                    break;
                                case RawType::U8:
                                    new_val.write_u8(0, static_cast<uint8_t>(dst_val));
                                    break;
                                case RawType::U16:
                                    new_val.write_u16(0, static_cast<uint16_t>(dst_val));
                                    break;
                                case RawType::U32:
                                    new_val.write_u32(0, dst_val);
                                    break;
                                case RawType::U64:
                                    new_val.write_u64(0, dst_val);
                                    break;
                                case RawType::ISize:
                                    new_val.write_usize(0, static_cast<int64_t>(dst_val));
                                    break;
                                case RawType::I8:
                                    new_val.write_i8(0, static_cast<int8_t>(dst_val));
                                    break;
                                case RawType::I16:
                                    new_val.write_i16(0, static_cast<int16_t>(dst_val));
                                    break;
                                case RawType::I32:
                                    new_val.write_i32(0, static_cast<int32_t>(dst_val));
                                    break;
                                case RawType::I64:
                                    new_val.write_i64(0, static_cast<int64_t>(dst_val));
                                    break;
                                default:
                                    throw "";
                                }
                                break;
                            case RawType::U128: throw "TODO"; /*dst_val = src_value.read_u128();*/ break;
                            case RawType::I128: throw "TODO"; /*dst_val = src_value.read_i128();*/ break;
                            }
                            } break;
                        case RawType::U128:
                        case RawType::I128:
                            LOG_TODO("Cast to " << re.type);
                        }
                    }
                    } break;
                TU_ARM(se.src, BinOp, re) {
                    ::HIR::TypeRef  ty_l, ty_r;
                    Value   tmp_l, tmp_r;
                    auto v_l = state.get_value_ref_param(re.val_l, tmp_l, ty_l);
                    auto v_r = state.get_value_ref_param(re.val_r, tmp_r, ty_r);
                    //LOG_DEBUG(v_l << " ? " << v_r);

                    switch(re.op)
                    {
                    case ::MIR::eBinOp::BIT_SHL:
                    case ::MIR::eBinOp::BIT_SHR:
                        LOG_TODO("BinOp SHL/SHR - can have mismatched types - " << se.src);
                    case ::MIR::eBinOp::EQ:
                    case ::MIR::eBinOp::NE:
                    case ::MIR::eBinOp::GT:
                    case ::MIR::eBinOp::GE:
                    case ::MIR::eBinOp::LT:
                    case ::MIR::eBinOp::LE: {
                        LOG_ASSERT(ty_l == ty_r, "BinOp type mismatch - " << ty_l << " != " << ty_r);
                        int res = 0;
                        // TODO: Handle comparison of the relocations too

                        const auto& alloc_l = v_l.m_value ? v_l.m_value->allocation : v_l.m_alloc;
                        const auto& alloc_r = v_r.m_value ? v_r.m_value->allocation : v_r.m_alloc;
                        auto reloc_l = alloc_l ? alloc_l.alloc().get_relocation(0) : AllocationPtr();
                        auto reloc_r = alloc_r ? alloc_r.alloc().get_relocation(0) : AllocationPtr();

                        if( reloc_l != reloc_r )
                        {
                            res = (reloc_l < reloc_r ? -1 : 1);
                        }
                        LOG_DEBUG("res=" << res << ", " << reloc_l << " ? " << reloc_r);

                        if( ty_l.wrappers.empty() )
                        {
                            switch(ty_l.inner_type)
                            {
                            case RawType::U64:
                                res = res != 0 ? res : Ops::do_compare(v_l.read_u64(0), v_r.read_u64(0));
                                break;
                            case RawType::U32:
                                res = res != 0 ? res : Ops::do_compare(v_l.read_u32(0), v_r.read_u32(0));
                                break;
                            case RawType::U16:
                                res = res != 0 ? res : Ops::do_compare(v_l.read_u16(0), v_r.read_u16(0));
                                break;
                            case RawType::U8:
                                res = res != 0 ? res : Ops::do_compare(v_l.read_u8(0), v_r.read_u8(0));
                                break;
                            case RawType::USize:
                                res = res != 0 ? res : Ops::do_compare(v_l.read_usize(0), v_r.read_usize(0));
                                break;
                            default:
                                LOG_TODO("BinOp comparisons - " << se.src << " w/ " << ty_l);
                            }
                        }
                        else if( ty_l.wrappers.front().type == TypeWrapper::Ty::Pointer )
                        {
                            // TODO: Technically only EQ/NE are valid.

                            res = res != 0 ? res : Ops::do_compare(v_l.read_usize(0), v_r.read_usize(0));

                            // Compare fat metadata.
                            if( res == 0 && v_l.m_size > POINTER_SIZE )
                            {
                                reloc_l = v_l.m_alloc ? v_l.m_alloc.alloc().get_relocation(POINTER_SIZE) : AllocationPtr();
                                reloc_r = v_r.m_alloc ? v_r.m_alloc.alloc().get_relocation(POINTER_SIZE) : AllocationPtr();

                                if( res == 0 && reloc_l != reloc_r )
                                {
                                    res = (reloc_l < reloc_r ? -1 : 1);
                                }
                                res = res != 0 ? res : Ops::do_compare(v_l.read_usize(POINTER_SIZE), v_r.read_usize(POINTER_SIZE));
                            }
                        }
                        else
                        {
                            LOG_TODO("BinOp comparisons - " << se.src << " w/ " << ty_l);
                        }
                        bool res_bool;
                        switch(re.op)
                        {
                        case ::MIR::eBinOp::EQ: res_bool = (res == 0);  break;
                        case ::MIR::eBinOp::NE: res_bool = (res != 0);  break;
                        case ::MIR::eBinOp::GT: res_bool = (res == 1);  break;
                        case ::MIR::eBinOp::GE: res_bool = (res == 1 || res == 0);  break;
                        case ::MIR::eBinOp::LT: res_bool = (res == -1); break;
                        case ::MIR::eBinOp::LE: res_bool = (res == -1 || res == 0); break;
                            break;
                        default:
                            LOG_BUG("Unknown comparison");
                        }
                        new_val = Value(::HIR::TypeRef(RawType::Bool));
                        new_val.write_u8(0, res_bool ? 1 : 0);
                        } break;
                    default:
                        LOG_ASSERT(ty_l == ty_r, "BinOp type mismatch - " << ty_l << " != " << ty_r);
                        new_val = Value(ty_l);
                        switch(ty_l.inner_type)
                        {
                        case RawType::U128:
                            LOG_TODO("BinOp U128");
                        case RawType::U64:
                            new_val.write_u64( 0, Ops::do_unsigned(v_l.read_u64(0), v_r.read_u64(0), re.op) );
                            break;
                        case RawType::U32:
                            new_val = Value(ty_l);
                            new_val.write_u32( 0, Ops::do_unsigned(v_l.read_u32(0), v_r.read_u32(0), re.op) );
                            break;
                        case RawType::U16:
                            new_val = Value(ty_l);
                            new_val.write_u16( 0, Ops::do_unsigned(v_l.read_u16(0), v_r.read_u16(0), re.op) );
                            break;
                        case RawType::U8:
                            new_val = Value(ty_l);
                            new_val.write_u8 ( 0, Ops::do_unsigned(v_l.read_u8 (0), v_r.read_u8 (0), re.op) );
                            break;
                        case RawType::USize:
                            new_val = Value(ty_l);
                            new_val.write_usize( 0, Ops::do_unsigned(v_l.read_usize(0), v_r.read_usize(0), re.op) );
                            break;
                        default:
                            LOG_TODO("Handle BinOp - w/ type " << ty_l);
                        }
                        break;
                    }
                    } break;
                TU_ARM(se.src, UniOp, re) {
                    ::HIR::TypeRef  ty;
                    auto v = state.get_value_and_type(re.val, ty);
                    LOG_ASSERT(ty.wrappers.empty(), "UniOp on wrapped type - " << ty);
                    new_val = Value(ty);
                    switch(re.op)
                    {
                    case ::MIR::eUniOp::INV:
                        switch(ty.inner_type)
                        {
                        case RawType::U128:
                            LOG_TODO("UniOp::INV U128");
                        case RawType::U64:
                            new_val.write_u64( 0, ~v.read_u64(0) );
                            break;
                        case RawType::U32:
                            new_val.write_u32( 0, ~v.read_u32(0) );
                            break;
                        case RawType::U16:
                            new_val.write_u16( 0, ~v.read_u16(0) );
                            break;
                        case RawType::U8:
                            new_val.write_u8 ( 0, ~v.read_u8 (0) );
                            break;
                        case RawType::USize:
                            new_val.write_usize( 0, ~v.read_usize(0) );
                            break;
                        case RawType::Bool:
                            new_val.write_u8 ( 0, v.read_u8 (0) == 0 );
                            break;
                        default:
                            LOG_TODO("UniOp::INV - w/ type " << ty);
                        }
                        break;
                    case ::MIR::eUniOp::NEG:
                        switch(ty.inner_type)
                        {
                        case RawType::I128:
                            LOG_TODO("UniOp::NEG I128");
                        case RawType::I64:
                            new_val.write_i64( 0, -v.read_i64(0) );
                            break;
                        case RawType::I32:
                            new_val.write_i32( 0, -v.read_i32(0) );
                            break;
                        case RawType::I16:
                            new_val.write_i16( 0, -v.read_i16(0) );
                            break;
                        case RawType::I8:
                            new_val.write_i8 ( 0, -v.read_i8 (0) );
                            break;
                        case RawType::ISize:
                            new_val.write_isize( 0, -v.read_isize(0) );
                            break;
                        default:
                            LOG_TODO("UniOp::INV - w/ type " << ty);
                        }
                        break;
                    }
                    } break;
                TU_ARM(se.src, DstMeta, re) {
                    LOG_TODO(stmt);
                    } break;
                TU_ARM(se.src, DstPtr, re) {
                    LOG_TODO(stmt);
                    } break;
                TU_ARM(se.src, MakeDst, re) {
                    LOG_TODO(stmt);
                    } break;
                TU_ARM(se.src, Tuple, re) {
                    ::HIR::TypeRef  dst_ty;
                    state.get_value_and_type(se.dst, dst_ty);
                    new_val = Value(dst_ty);

                    for(size_t i = 0; i < re.vals.size(); i++)
                    {
                        auto fld_ofs = dst_ty.composite_type->fields.at(i).first;
                        new_val.write_value(fld_ofs, state.param_to_value(re.vals[i]));
                    }
                    } break;
                TU_ARM(se.src, Array, re) {
                    ::HIR::TypeRef  dst_ty;
                    state.get_value_and_type(se.dst, dst_ty);
                    new_val = Value(dst_ty);
                    // TODO: Assert that type is an array
                    auto inner_ty = dst_ty.get_inner();
                    size_t stride = inner_ty.get_size();

                    size_t ofs = 0;
                    for(const auto& v : re.vals)
                    {
                        new_val.write_value(ofs, state.param_to_value(v));
                        ofs += stride;
                    }
                    } break;
                TU_ARM(se.src, SizedArray, re) {
                    ::HIR::TypeRef  dst_ty;
                    state.get_value_and_type(se.dst, dst_ty);
                    new_val = Value(dst_ty);
                    // TODO: Assert that type is an array
                    auto inner_ty = dst_ty.get_inner();
                    size_t stride = inner_ty.get_size();

                    size_t ofs = 0;
                    for(size_t i = 0; i < re.count; i++)
                    {
                        new_val.write_value(ofs, state.param_to_value(re.val));
                        ofs += stride;
                    }
                    } break;
                TU_ARM(se.src, Variant, re) {
                    // 1. Get the composite by path.
                    const auto& data_ty = state.modtree.get_composite(re.path);
                    auto dst_ty = ::HIR::TypeRef(&data_ty);
                    new_val = Value(dst_ty);
                    // Three cases:
                    // - Unions (no tag)
                    // - Data enums (tag and data)
                    // - Value enums (no data)
                    const auto& var = data_ty.variants.at(re.index);
                    if( var.data_field != SIZE_MAX )
                    {
                        const auto& fld = data_ty.fields.at(re.index);

                        new_val.write_value(fld.first, state.param_to_value(re.val));
                    }
                    if( var.base_field != SIZE_MAX )
                    {
                        ::HIR::TypeRef  tag_ty;
                        size_t tag_ofs = dst_ty.get_field_ofs(var.base_field, var.field_path, tag_ty);
                        LOG_ASSERT(tag_ty.get_size() == var.tag_data.size(), "");
                        new_val.write_bytes(tag_ofs, var.tag_data.data(), var.tag_data.size());
                    }
                    else
                    {
                        // Union, no tag
                    }
                    } break;
                TU_ARM(se.src, Struct, re) {
                    const auto& data_ty = state.modtree.get_composite(re.path);

                    ::HIR::TypeRef  dst_ty;
                    state.get_value_and_type(se.dst, dst_ty);
                    new_val = Value(dst_ty);
                    LOG_ASSERT(dst_ty.composite_type == &data_ty, "Destination type of RValue::Struct isn't the same as the input");

                    for(size_t i = 0; i < re.vals.size(); i++)
                    {
                        auto fld_ofs = data_ty.fields.at(i).first;
                        new_val.write_value(fld_ofs, state.param_to_value(re.vals[i]));
                    }
                    } break;
                }
                LOG_DEBUG("- " << new_val);
                state.write_lvalue(se.dst, ::std::move(new_val));
                } break;
            case ::MIR::Statement::TAG_Asm:
                LOG_TODO(stmt);
                break;
            TU_ARM(stmt, Drop, se) {
                if( se.flag_idx == ~0u || state.drop_flags.at(se.flag_idx) )
                {
                    auto drop_value = [](ValueRef v, const ::HIR::TypeRef& ty) {
                        if( ty.wrappers.empty() )
                        {
                            if( ty.inner_type == RawType::Composite )
                            {
                                if( ty.composite_type->drop_glue != ::HIR::Path() )
                                {
                                    LOG_TODO("Drop - " << ty);
                                }
                                else
                                {
                                    // No drop glue
                                }
                            }
                            else if( ty.inner_type == RawType::TraitObject )
                            {
                                LOG_TODO("Drop - " << ty);
                            }
                            else
                            {
                                // No destructor
                            }
                        }
                        else if( ty.wrappers[0].type == TypeWrapper::Ty::Borrow )
                        {
                            if( ty.wrappers[0].size == static_cast<size_t>(::HIR::BorrowType::Move) )
                            {
                                LOG_TODO("Drop - " << ty << " - dereference and go to inner");
                                // TODO: Clear validity on the entire inner value.
                            }
                            else
                            {
                                // No destructor
                            }
                        }
                        // TODO: Arrays
                        else
                        {
                            LOG_TODO("Drop - " << ty);
                        }

                        };

                    ::HIR::TypeRef  ty;
                    auto v = state.get_value_and_type(se.slot, ty);
                    drop_value(v, ty);
                    // TODO: Clear validity on the entire inner value.
                }
                } break;
            TU_ARM(stmt, SetDropFlag, se) {
                bool val = (se.other == ~0 ? false : state.drop_flags.at(se.other)) != se.new_val;
                LOG_DEBUG("- " << val);
                state.drop_flags.at(se.idx) = val;
                } break;
            case ::MIR::Statement::TAG_ScopeEnd:
                LOG_TODO(stmt);
                break;
            }
        }

        LOG_DEBUG("BB" << bb_idx << "/TERM: " << bb.terminator);
        switch(bb.terminator.tag())
        {
        case ::MIR::Terminator::TAGDEAD:    throw "";
        TU_ARM(bb.terminator, Incomplete, _te)
            LOG_TODO("Terminator::Incomplete hit");
        TU_ARM(bb.terminator, Diverge, _te)
            LOG_TODO("Terminator::Diverge hit");
        TU_ARM(bb.terminator, Panic, _te)
            LOG_TODO("Terminator::Panic");
        TU_ARM(bb.terminator, Goto, te)
            bb_idx = te;
            continue;
        TU_ARM(bb.terminator, Return, _te)
            return state.ret;
        TU_ARM(bb.terminator, If, te) {
            uint8_t v = state.get_value_ref(te.cond).read_u8(0);
            LOG_ASSERT(v == 0 || v == 1, "");
            bb_idx = v ? te.bb0 : te.bb1;
            } continue;
        TU_ARM(bb.terminator, Switch, te) {
            ::HIR::TypeRef ty;
            auto v = state.get_value_and_type(te.val, ty);
            LOG_ASSERT(ty.wrappers.size() == 0, "" << ty);
            LOG_ASSERT(ty.inner_type == RawType::Composite, "" << ty);

            // TODO: Convert the variant list into something that makes it easier to switch on.
            size_t found_target = SIZE_MAX;
            size_t default_target = SIZE_MAX;
            for(size_t i = 0; i < ty.composite_type->variants.size(); i ++)
            {
                const auto& var = ty.composite_type->variants[i];
                if( var.tag_data.size() == 0 )
                {
                    // Save as the default, error for multiple defaults
                    if( default_target != SIZE_MAX )
                    {
                        LOG_FATAL("Two variants with no tag in Switch");
                    }
                    default_target = i;
                }
                else
                {
                    // Get offset, read the value.
                    ::HIR::TypeRef  tag_ty;
                    size_t tag_ofs = ty.get_field_ofs(var.base_field, var.field_path, tag_ty);
                    // Read the value bytes
                    ::std::vector<char> tmp( var.tag_data.size() );
                    v.read_bytes(tag_ofs, const_cast<char*>(tmp.data()), tmp.size());
                    if( ::std::memcmp(tmp.data(), var.tag_data.data(), tmp.size()) == 0 )
                    {
                        found_target = i;
                        break ;
                    }
                }
            }

            if( found_target == SIZE_MAX )
            {
                found_target = default_target;
            }
            if( found_target == SIZE_MAX )
            {
                LOG_FATAL("Terminator::Switch on " << ty << " didn't find a variant");
            }
            bb_idx = te.targets.at(found_target);
            } continue;
        TU_ARM(bb.terminator, SwitchValue, _te)
            LOG_TODO("Terminator::SwitchValue");
        TU_ARM(bb.terminator, Call, te) {
            ::std::vector<Value>    sub_args; sub_args.reserve(te.args.size());
            for(const auto& a : te.args)
            {
                sub_args.push_back( state.param_to_value(a) );
            }
            if( te.fcn.is_Intrinsic() )
            {
                const auto& fe = te.fcn.as_Intrinsic();
                state.write_lvalue(te.ret_val, MIRI_Invoke_Intrinsic(fe.name, fe.params, ::std::move(sub_args)));
            }
            else
            {
                const ::HIR::Path* fcn_p;
                if( te.fcn.is_Path() ) {
                    fcn_p = &te.fcn.as_Path();
                }
                else {
                    ::HIR::TypeRef ty;
                    auto v = state.get_value_and_type(te.fcn.as_Value(), ty);
                    // TODO: Assert type
                    // TODO: Assert offset/content.
                    assert(v.read_usize(v.m_offset) == 0);
                    auto& alloc_ptr = v.m_alloc ? v.m_alloc : v.m_value->allocation;
                    LOG_ASSERT(alloc_ptr, "");
                    auto& fcn_alloc_ptr = alloc_ptr.alloc().get_relocation(v.m_offset);
                    LOG_ASSERT(fcn_alloc_ptr, "");
                    fcn_p = &fcn_alloc_ptr.fcn();
                }

                LOG_DEBUG("Call " << *fcn_p);
                state.write_lvalue(te.ret_val, MIRI_Invoke(modtree, *fcn_p, ::std::move(sub_args)));
            }
            bb_idx = te.ret_block;
            } continue;
        }
        throw "";
    }

    throw "";
}
Value MIRI_Invoke_Extern(const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args)
{
    if( link_name == "AddVectoredExceptionHandler" )
    {
        LOG_DEBUG("Call `AddVectoredExceptionHandler` - Ignoring and returning non-null");
        auto rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, 1);
        return rv;
    }
    else
    {
        LOG_TODO("Call external function " << link_name);
    }
}
Value MIRI_Invoke_Intrinsic(const ::std::string& name, const ::HIR::PathParams& ty_params, ::std::vector<Value> args)
{
    Value rv;
    TRACE_FUNCTION_R(name, rv);
    if( name == "atomic_store" )
    {
        auto& ptr_val = args.at(0);
        auto& data_val = args.at(1);

        LOG_ASSERT(ptr_val.size() == POINTER_SIZE, "atomic_store of a value that isn't a pointer-sized value");

        // There MUST be a relocation at this point with a valid allocation.
        LOG_ASSERT(ptr_val.allocation, "Deref of a value with no allocation (hence no relocations)");
        LOG_TRACE("Deref " << ptr_val.allocation.alloc());
        auto alloc = ptr_val.allocation.alloc().get_relocation(0);
        LOG_ASSERT(alloc, "Deref of a value with no relocation");

        // TODO: Atomic side of this?
        size_t ofs = ptr_val.read_usize(0);
        const auto& ty = ty_params.tys.at(0);
        alloc.alloc().write_value(ofs, ::std::move(data_val));
    }
    else if( name == "atomic_load" )
    {
        auto& ptr_val = args.at(0);
        LOG_ASSERT(ptr_val.size() == POINTER_SIZE, "atomic_store of a value that isn't a pointer-sized value");

        // There MUST be a relocation at this point with a valid allocation.
        LOG_ASSERT(ptr_val.allocation, "Deref of a value with no allocation (hence no relocations)");
        LOG_TRACE("Deref " << ptr_val.allocation.alloc());
        auto alloc = ptr_val.allocation.alloc().get_relocation(0);
        LOG_ASSERT(alloc, "Deref of a value with no relocation");

        // TODO: Atomic side of this?
        size_t ofs = ptr_val.read_usize(0);
        const auto& ty = ty_params.tys.at(0);
        rv = alloc.alloc().read_value(ofs, ty.get_size());
    }
    else if( name == "transmute" )
    {
        // Transmute requires the same size, so just copying the value works
        rv = ::std::move(args.at(0));
    }
    else if( name == "assume" )
    {
        // Assume is a no-op which returns unit
    }
    else if( name == "offset" )
    {
        auto ptr_val = ::std::move(args.at(0));
        auto& ofs_val = args.at(1);

        auto r = ptr_val.allocation.alloc().get_relocation(0);
        auto orig_ofs = ptr_val.read_usize(0);
        auto delta_ofs = ptr_val.read_usize(0);
        auto new_ofs = orig_ofs + delta_ofs;
        if(POINTER_SIZE != 8) {
            new_ofs &= 0xFFFFFFFF;
        }


        ptr_val.write_usize(0, new_ofs);
        ptr_val.allocation.alloc().relocations.push_back({ 0, r });
        return ptr_val;
    }
    else
    {
        LOG_TODO("Call intrinsic \"" << name << "\"");
    }
    return rv;
}

int ProgramOptions::parse(int argc, const char* argv[])
{
    bool all_free = false;
    for(int argidx = 1; argidx < argc; argidx ++)
    {
        const char* arg = argv[argidx]; 
        if( arg[0] != '-' || all_free )
        {
            // Free
            if( this->infile == "" )
            {
                this->infile = arg;
            }
            else
            {
                // TODO: Too many free arguments
            }
        }
        else if( arg[1] != '-' )
        {
            // Short
        }
        else if( arg[2] != '\0' )
        {
            // Long
        }
        else
        {
            all_free = true;
        }
    }
    return 0;
}
