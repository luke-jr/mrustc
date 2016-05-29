/*
 */
#include "main_bindings.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <algorithm>    // std::find_if

namespace {
    
    bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
    
    bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl)
    {
        for(const auto& ty : tpl.m_types)
            if( monomorphise_type_needed(ty) )
                return true;
        return false;
    }
    bool monomorphise_path_needed(const ::HIR::Path& tpl)
    {
        TU_MATCH(::HIR::Path::Data, (tpl.m_data), (e),
        (Generic,
            return monomorphise_pathparams_needed(e.m_params);
            ),
        (UfcsInherent,
            return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
            ),
        (UfcsKnown,
            return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.trait.m_params) || monomorphise_pathparams_needed(e.params);
            ),
        (UfcsUnknown,
            return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
            )
        )
        throw "";
    }
    bool monomorphise_type_needed(const ::HIR::TypeRef& tpl)
    {
        TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
        (Infer,
            assert(!"ERROR: _ type found in monomorphisation target");
            ),
        (Diverge,
            return false;
            ),
        (Primitive,
            return false;
            ),
        (Path,
            return monomorphise_path_needed(e.path);
            ),
        (Generic,
            return true;
            ),
        (TraitObject,
            TODO(Span(), "TraitObject - " << tpl);
            ),
        (Array,
            TODO(Span(), "Array - " << tpl);
            ),
        (Slice,
            return monomorphise_type_needed(*e.inner);
            ),
        (Tuple,
            for(const auto& ty : e) {
                if( monomorphise_type_needed(ty) )
                    return true;
            }
            return false;
            ),
        (Borrow,
            return monomorphise_type_needed(*e.inner);
            ),
        (Pointer,
            return monomorphise_type_needed(*e.inner);
            ),
        (Function,
            TODO(Span(), "Function - " << tpl);
            )
        )
        throw "";
    }
    //::HIR::Path monomorphise_path(const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::Path& tpl)
    //{
    //}
    ::HIR::TypeRef monomorphise_type(const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::TypeRef& tpl)
    {
        TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
        (Infer,
            assert(!"ERROR: _ type found in monomorphisation target");
            ),
        (Diverge,
            return ::HIR::TypeRef(e);
            ),
        (Primitive,
            return ::HIR::TypeRef(e);
            ),
        (Path,
            TODO(Span(), "Path");
            ),
        (Generic,
            if( e.binding >= params.m_types.size() ) {
                BUG(Span(), "Generic param out of range");
            }
            return params.m_types[e.binding].clone();
            ),
        (TraitObject,
            TODO(Span(), "TraitObject");
            ),
        (Array,
            TODO(Span(), "Array");
            ),
        (Slice,
            return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Slice({ box$(monomorphise_type(params_def, params, *e.inner)) }) );
            ),
        (Tuple,
            ::std::vector< ::HIR::TypeRef>  types;
            for(const auto& ty : e) {
                types.push_back( monomorphise_type(params_def, params,  ty) );
            }
            return ::HIR::TypeRef( mv$(types) );
            ),
        (Borrow,
            return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Borrow({ e.type, box$(monomorphise_type(params_def, params,  *e.inner)) }) );
            ),
        (Pointer,
            return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Pointer({ e.is_mut, box$(monomorphise_type(params_def, params,  *e.inner)) }) );
            ),
        (Function,
            TODO(Span(), "Function");
            )
        )
        throw "";
    }
    
    struct IVar
    {
        unsigned int alias;
        ::std::unique_ptr< ::HIR::TypeRef> type;
        
        IVar():
            alias(~0u),
            type(new ::HIR::TypeRef())
        {}
        bool is_alias() const { return alias != ~0u; }
    };
    
    class TypecheckContext
    {
        ::std::vector< IVar >   m_ivars;
        bool    m_has_changed;
    public:
        TypecheckContext(const ::HIR::TypeRef& result_type):
            m_has_changed(false)
        {
        }
        
        bool take_changed() {
            bool rv = m_has_changed;
            m_has_changed = false;
            return rv;
        }
        void mark_change() {
            m_has_changed = true;
        }
        
        /// Adds a local variable binding (type is mutable so it can be inferred if required)
        void add_local(unsigned int index, ::HIR::TypeRef type)
        {
        }
        /// Add (and bind) all '_' types in `type`
        void add_ivars(::HIR::TypeRef& type)
        {
            TU_MATCH(::HIR::TypeRef::Data, (type.m_data), (e),
            (Infer,
                e.index = this->new_ivar();
                ),
            (Diverge,
                ),
            (Primitive,
                ),
            (Path,
                // Iterate all arguments
                ),
            (Generic,
                ),
            (TraitObject,
                // Iterate all paths
                ),
            (Array,
                add_ivars(*e.inner);
                ),
            (Slice,
                add_ivars(*e.inner);
                ),
            (Tuple,
                for(auto& ty : e)
                    add_ivars(ty);
                ),
            (Borrow,
                add_ivars(*e.inner);
                ),
            (Pointer,
                add_ivars(*e.inner);
                ),
            (Function,
                // No ivars allowed
                // TODO: Check?
                )
            )
        }
        void add_ivars_params(::HIR::PathParams& params)
        {
            for(auto& arg : params.m_types)
                add_ivars(arg);
        }
        
        
        void add_pattern_binding(const ::HIR::PatternBinding& pb, ::HIR::TypeRef type)
        {
            assert( pb.is_valid() );
            switch( pb.m_type )
            {
            case ::HIR::PatternBinding::Type::Move:
                this->add_local( pb.m_slot, mv$(type) );
                break;
            case ::HIR::PatternBinding::Type::Ref:
                this->add_local( pb.m_slot,  ::HIR::TypeRef::Data::make_Borrow( {::HIR::BorrowType::Shared, box$(mv$(type))} ) );
                break;
            case ::HIR::PatternBinding::Type::MutRef:
                this->add_local( pb.m_slot,  ::HIR::TypeRef::Data::make_Borrow( {::HIR::BorrowType::Unique, box$(mv$(type))} ) );
                break;
            }
        }
        
        void add_binding(::HIR::Pattern& pat, ::HIR::TypeRef& type)
        {
            static Span _sp;
            const Span& sp = _sp;
            
            if( pat.m_binding.is_valid() ) {
                this->add_pattern_binding(pat.m_binding, type.clone());
                // TODO: Can there be bindings within a bound pattern?
                //return ;
            }
            
            // 
            TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
            (Any,
                // Just leave it, the pattern says nothing
                ),
            (Value,
                //TODO(sp, "Value pattern");
                ),
            (Range,
                //TODO(sp, "Range pattern");
                ),
            (Box,
                TODO(sp, "Box pattern");
                ),
            (Ref,
                if( type.m_data.is_Infer() ) {
                    type.m_data = ::HIR::TypeRef::Data::make_Borrow({ e.type, box$(this->new_ivar_tr()) });
                }
                // Type must be a &-ptr
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Borrow,
                    if( te.type != e.type ) {
                        // TODO: Type mismatch
                    }
                    this->add_binding( *e.sub, *te.inner );
                    )
                )
                ),
            (Tuple,
                if( type.m_data.is_Infer() ) {
                    ::std::vector< ::HIR::TypeRef>  sub_types;
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        sub_types.push_back( this->new_ivar_tr() );
                    type.m_data = ::HIR::TypeRef::Data::make_Tuple( mv$(sub_types) );
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Tuple,
                    if( te.size() != e.sub_patterns.size() ) {
                        // TODO: Type mismatch
                    }
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        this->add_binding( e.sub_patterns[i], te[i] );
                    )
                )
                ),
            (Slice,
                if( type.m_data.is_Infer() ) {
                    type.m_data = ::HIR::TypeRef::Data::make_Slice( {box$(this->new_ivar_tr())} );
                    this->mark_change();
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw""; ),
                (Slice,
                    for(auto& sp : e.sub_patterns)
                        this->add_binding( sp, *te.inner );
                    )
                )
                ),
            (SplitSlice,
                if( type.m_data.is_Infer() ) {
                    type.m_data = ::HIR::TypeRef::Data::make_Slice( {box$(this->new_ivar_tr())} );
                    this->mark_change();
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Slice,
                    for(auto& sp : e.leading)
                        this->add_binding( sp, *te.inner );
                    for(auto& sp : e.trailing)
                        this->add_binding( sp, *te.inner );
                    if( e.extra_bind.is_valid() ) {
                        this->add_local( e.extra_bind.m_slot, type.clone() );
                    }
                    )
                )
                ),
            
            // - Enums/Structs
            (StructTuple,
                this->add_ivars_params( e.path.m_params );
                if( type.m_data.is_Infer() ) {
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
                }
                assert(e.binding);
                const auto& str = *e.binding;
                // - assert check from earlier pass
                assert( str.m_data.is_Tuple() );
                const auto& sd = str.m_data.as_Tuple();
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Path,
                    if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                        ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
                    }
                    // NOTE: Must be Generic for the above to have passed
                    auto& gp = te.path.m_data.as_Generic();
                    
                    if( e.sub_patterns.size() != sd.size() ) { 
                        ERROR(sp, E0000, "Tuple struct pattern with an incorrect number of fields");
                    }
                    for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                    {
                        const auto& field_type = sd[i].ent;
                        if( monomorphise_type_needed(field_type) ) {
                            auto var_ty = monomorphise_type(str.m_params, gp.m_params,  field_type);
                            this->add_binding(e.sub_patterns[i], var_ty);
                        }
                        else {
                            // SAFE: Can't have _ as monomorphise_type_needed checks for that
                            this->add_binding(e.sub_patterns[i], const_cast< ::HIR::TypeRef&>(field_type));
                        }
                    }
                    )
                )
                ),
            (StructTupleWildcard,
                this->add_ivars_params( e.path.m_params );
                if( type.m_data.is_Infer() ) {
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
                }
                assert(e.binding);
                const auto& str = *e.binding;
                // - assert check from earlier pass
                assert( str.m_data.is_Tuple() );
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Path,
                    if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                        ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
                    }
                    )
                )
                ),
            (Struct,
                this->add_ivars_params( e.path.m_params );
                if( type.m_data.is_Infer() ) {
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
                }
                assert(e.binding);
                const auto& str = *e.binding;
                // - assert check from earlier pass
                assert( str.m_data.is_Named() );
                const auto& sd = str.m_data.as_Named();
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Path,
                    if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                        ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
                    }
                    // NOTE: Must be Generic for the above to have passed
                    auto& gp = te.path.m_data.as_Generic();
                    for( auto& field_pat : e.sub_patterns )
                    {
                        unsigned int f_idx = ::std::find_if( sd.begin(), sd.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - sd.begin();
                        if( f_idx == sd.size() ) {
                            ERROR(sp, E0000, "Struct " << e.path << " doesn't have a field " << field_pat.first);
                        }
                        const ::HIR::TypeRef& field_type = sd[f_idx].second.ent;
                        if( monomorphise_type_needed(field_type) ) {
                            auto field_type_mono = monomorphise_type(str.m_params, gp.m_params,  field_type);
                            this->add_binding(field_pat.second, field_type_mono);
                        }
                        else {
                            // SAFE: Can't have _ as monomorphise_type_needed checks for that
                            this->add_binding(field_pat.second, const_cast< ::HIR::TypeRef&>(field_type));
                        }
                    }
                    )
                )
                ),
            (EnumTuple,
                this->add_ivars_params( e.path.m_params );
                if( type.m_data.is_Infer() ) {
                    auto path = e.path.clone();
                    path.m_path.m_components.pop_back();
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)} );
                }
                assert(e.binding_ptr);
                const auto& enm = *e.binding_ptr;
                const auto& var = enm.m_variants[e.binding_idx].second;
                assert(var.is_Tuple());
                const auto& tup_var = var.as_Tuple();
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Path,
                    if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                        ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
                    }
                    // NOTE: Must be Generic for the above to have passed
                    auto& gp = te.path.m_data.as_Generic();
                    if( e.sub_patterns.size() != tup_var.size() ) { 
                        ERROR(sp, E0000, "Enum pattern with an incorrect number of fields - " << e.path << " - expected " << tup_var.size() << ", got " << e.sub_patterns.size());
                    }
                    for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                    {
                        if( monomorphise_type_needed(tup_var[i]) ) {
                            auto var_ty = monomorphise_type(enm.m_params, gp.m_params,  tup_var[i]);
                            this->add_binding(e.sub_patterns[i], var_ty);
                        }
                        else {
                            // SAFE: Can't have a _ (monomorphise_type_needed checks for that)
                            this->add_binding(e.sub_patterns[i], const_cast< ::HIR::TypeRef&>(tup_var[i]));
                        }
                    }
                    )
                )
                ),
            (EnumTupleWildcard,
                this->add_ivars_params( e.path.m_params );
                if( type.m_data.is_Infer() ) {
                    auto path = e.path.clone();
                    path.m_path.m_components.pop_back();
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)} );
                }
                assert(e.binding_ptr);
                const auto& enm = *e.binding_ptr;
                const auto& var = enm.m_variants[e.binding_idx].second;
                assert(var.is_Tuple());
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Path,
                    if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                        ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
                    }
                    )
                )
                ),
            (EnumStruct,
                this->add_ivars_params( e.path.m_params );
                if( type.m_data.is_Infer() ) {
                    auto path = e.path.clone();
                    path.m_path.m_components.pop_back();
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)} );
                }
                assert(e.binding_ptr);
                const auto& enm = *e.binding_ptr;
                const auto& var = enm.m_variants[e.binding_idx].second;
                assert(var.is_Struct());
                const auto& tup_var = var.as_Struct();
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Path,
                    if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                        ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
                    }
                    // NOTE: Must be Generic for the above to have passed
                    auto& gp = te.path.m_data.as_Generic();
                    
                    for( auto& field_pat : e.sub_patterns )
                    {
                        unsigned int f_idx = ::std::find_if( tup_var.begin(), tup_var.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - tup_var.begin();
                        if( f_idx == tup_var.size() ) {
                            ERROR(sp, E0000, "Enum variant " << e.path << " doesn't have a field " << field_pat.first);
                        }
                        const ::HIR::TypeRef& field_type = tup_var[f_idx].second;
                        if( monomorphise_type_needed(field_type) ) {
                            auto field_type_mono = monomorphise_type(enm.m_params, gp.m_params,  field_type);
                            this->add_binding(field_pat.second, field_type_mono);
                        }
                        else {
                            // SAFE: Can't have _ as monomorphise_type_needed checks for that
                            this->add_binding(field_pat.second, const_cast< ::HIR::TypeRef&>(field_type));
                        }
                    }
                    )
                )
                )
            )
        }
        
        /// Run inferrence using a pattern
        void apply_pattern(const ::HIR::Pattern& pat, ::HIR::TypeRef& type)
        {
            static Span _sp;
            const Span& sp = _sp;
            // TODO: Should this do an equality on the binding?

            auto& ty = this->get_type(type);
            
            TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
            (Any,
                // Just leave it, the pattern says nothing about the type
                ),
            (Value,
                TODO(sp, "Value pattern");
                ),
            (Range,
                TODO(sp, "Range pattern");
                ),
            // - Pointer destructuring
            (Box,
                // Type must be box-able
                TODO(sp, "Box patterns");
                ),
            (Ref,
                if( ty.m_data.is_Infer() ) {
                    BUG(sp, "Infer type hit that should already have been fixed");
                }
                // Type must be a &-ptr
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Borrow,
                    if( te.type != e.type ) {
                        // TODO: Type mismatch
                    }
                    this->apply_pattern( *e.sub, *te.inner );
                    )
                )
                ),
            (Tuple,
                if( ty.m_data.is_Infer() ) {
                    BUG(sp, "Infer type hit that should already have been fixed");
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Tuple,
                    if( te.size() != e.sub_patterns.size() ) {
                        // TODO: Type mismatch
                    }
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        this->apply_pattern( e.sub_patterns[i], te[i] );
                    )
                )
                ),
            // --- Slices
            (Slice,
                if( ty.m_data.is_Infer() ) {
                    BUG(sp, "Infer type hit that should already have been fixed");
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Slice,
                    for(const auto& sp : e.sub_patterns )
                        this->apply_pattern( sp, *te.inner );
                    )
                )
                ),
            (SplitSlice,
                if( ty.m_data.is_Infer() ) {
                    BUG(sp, "Infer type hit that should already have been fixed");
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Slice,
                    for(const auto& sp : e.leading)
                        this->apply_pattern( sp, *te.inner );
                    for(const auto& sp : e.trailing)
                        this->apply_pattern( sp, *te.inner );
                    // TODO: extra_bind? (see comment at start of function)
                    )
                )
                ),
            
            // - Enums/Structs
            (StructTuple,
                if( ty.m_data.is_Infer() ) {
                    ty.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    // TODO: Does anything need to happen here? This can only introduce equalities?
                    )
                )
                ),
            (StructTupleWildcard,
                ),
            (Struct,
                if( ty.m_data.is_Infer() ) {
                    //TODO: Does this lead to issues with generic parameters?
                    ty.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    // TODO: Does anything need to happen here? This can only introduce equalities?
                    )
                )
                ),
            (EnumTuple,
                if( ty.m_data.is_Infer() ) {
                    TODO(sp, "EnumTuple - infer");
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    )
                )
                ),
            (EnumTupleWildcard,
                ),
            (EnumStruct,
                if( ty.m_data.is_Infer() ) {
                    TODO(sp, "EnumStruct - infer");
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    )
                )
                )
            )
        }
        // Adds a rule that two types must be equal
        void apply_equality(const ::HIR::TypeRef& left, const ::HIR::TypeRef& right)
        {
        }
    public:
        unsigned int new_ivar()
        {
            m_ivars.push_back( IVar() );
            return m_ivars.size() - 1;
        }
        ::HIR::TypeRef new_ivar_tr() {
            ::HIR::TypeRef rv;
            rv.m_data.as_Infer().index = this->new_ivar();
            return rv;
        }
        
        ::HIR::TypeRef& get_type(::HIR::TypeRef& type)
        {
            TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
                auto index = e.index;
                while( m_ivars.at(index).is_alias() ) {
                    index = m_ivars.at(index).alias;
                }
                return *m_ivars.at(index).type;
            )
            else {
                return type;
            }
        }
    };
    
    class ExprVisitor_Enum:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
    public:
        ExprVisitor_Enum(TypecheckContext& context):
            context(context)
        {
        }
        
        void visit_node(::HIR::ExprNode& node) override {
            this->context.add_ivars(node.m_res_type);
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            this->context.add_ivars(node.m_type);
            
            this->context.add_binding(node.m_pattern, node.m_type);
        }
        
        void visit(::HIR::ExprNode_Match& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            
            for(auto& arm : node.m_arms)
            {
                for(auto& pat : arm.m_patterns)
                {
                    this->context.add_binding(pat, node.m_res_type);
                }
            }
        }
    };
    
    class ExprVisitor_Run:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
    public:
        ExprVisitor_Run(TypecheckContext& context):
            context(context)
        {
        }
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            this->context.apply_pattern(node.m_pattern, node.m_type);
            //if( node.m_value ) {
            //    this->context.add_rule_equality(node.m_type, node.m_value->m_res_type);
            //}
        }
        
    };
}

void Typecheck_Code(TypecheckContext context, ::HIR::ExprNode& root_node)
{
    TRACE_FUNCTION;
    
    // 1. Enumerate inferrence variables and assign indexes to them
    {
        ExprVisitor_Enum    visitor { context };
        root_node.visit( visitor );
    }
    // 2. Iterate through nodes applying rules until nothing changes
    {
        ExprVisitor_Run visitor { context };
        do {
            root_node.visit( visitor );
        } while( context.take_changed() );
    }
}



namespace {
    class OuterVisitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& crate;
        
        ::HIR::GenericParams*   m_impl_generics;
        ::HIR::GenericParams*   m_item_generics;
    public:
        OuterVisitor(::HIR::Crate& crate):
            crate(crate),
            m_impl_generics(nullptr),
            m_item_generics(nullptr)
        {
        }
        
    private:
        template<typename T>
        class NullOnDrop {
            T*& ptr;
        public:
            NullOnDrop(T*& ptr):
                ptr(ptr)
            {}
            ~NullOnDrop() {
                ptr = nullptr;
            }
        };
        NullOnDrop< ::HIR::GenericParams> set_impl_generics(::HIR::GenericParams& gps) {
            assert( !m_impl_generics );
            m_impl_generics = &gps;
            return NullOnDrop< ::HIR::GenericParams>(m_impl_generics);
        }
        NullOnDrop< ::HIR::GenericParams> set_item_generics(::HIR::GenericParams& gps) {
            assert( !m_item_generics );
            m_item_generics = &gps;
            return NullOnDrop< ::HIR::GenericParams>(m_item_generics);
        }
    
    public:
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) {
            TODO(Span(), "visit_expr");
        }

        void visit_trait(::HIR::Trait& item) override
        {
            //::HIR::TypeRef tr { "Self", 0 };
            auto _ = this->set_impl_generics(item.m_params);
            //m_self_types.push_back(&tr);
            ::HIR::Visitor::visit_trait(item);
            //m_self_types.pop_back();
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            //m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_type_impl(impl);
            // Check that the type is valid
            
            //m_self_types.pop_back();
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            //m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            //m_self_types.pop_back();
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = this->set_impl_generics(impl.m_params);
            //m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            //m_self_types.pop_back();
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                TypecheckContext    typeck_context { ::HIR::TypeRef(::HIR::CoreType::Usize) };
                Typecheck_Code( mv$(typeck_context), *e.size );
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::Function& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( &*item.m_code )
            {
                TypecheckContext typeck_context { item.m_return };
                for( auto& arg : item.m_args ) {
                    typeck_context.add_binding( arg.first, arg.second );
                }
                Typecheck_Code( mv$(typeck_context), *item.m_code );
            }
        }
        void visit_static(::HIR::Static& item) override {
            //auto _ = this->set_item_generics(item.m_params);
            if( &*item.m_value )
            {
                TypecheckContext typeck_context { item.m_type };
                Typecheck_Code( mv$(typeck_context), *item.m_value );
            }
        }
        void visit_constant(::HIR::Constant& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( &*item.m_value )
            {
                TypecheckContext typeck_context { item.m_type };
                Typecheck_Code( mv$(typeck_context), *item.m_value );
            }
        }
        void visit_enum(::HIR::Enum& item) override {
            auto _ = this->set_item_generics(item.m_params);
            
            // TODO: Use a different type depding on repr()
            auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Usize);
            
            // TODO: Check types too?
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    TypecheckContext typeck_context { enum_type };
                    Typecheck_Code( mv$(typeck_context), *e );
                )
            }
        }
    };
}

void Typecheck_Expressions(::HIR::Crate& crate)
{
    OuterVisitor    visitor { crate };
    visitor.visit_crate( crate );
}