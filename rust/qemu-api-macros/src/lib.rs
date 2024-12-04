// Copyright 2024, Linaro Limited
// Author(s): Manos Pitsidianakis <manos.pitsidianakis@linaro.org>
// SPDX-License-Identifier: GPL-2.0-or-later

use proc_macro::TokenStream;
use quote::quote;
use syn::{
    parse_macro_input, parse_quote, punctuated::Punctuated, token::Comma, Data, DeriveInput, Field,
    Fields, Ident, Type, Visibility,
};

mod utils;
use utils::MacroError;

fn get_fields<'a>(
    input: &'a DeriveInput,
    msg: &str,
) -> Result<&'a Punctuated<Field, Comma>, MacroError> {
    if let Data::Struct(s) = &input.data {
        if let Fields::Named(fs) = &s.fields {
            Ok(&fs.named)
        } else {
            Err(MacroError::Message(
                format!("Named fields required for {}", msg),
                input.ident.span(),
            ))
        }
    } else {
        Err(MacroError::Message(
            format!("Struct required for {}", msg),
            input.ident.span(),
        ))
    }
}

fn is_c_repr(input: &DeriveInput, msg: &str) -> Result<(), MacroError> {
    let expected = parse_quote! { #[repr(C)] };

    if input.attrs.iter().any(|attr| attr == &expected) {
        Ok(())
    } else {
        Err(MacroError::Message(
            format!("#[repr(C)] required for {}", msg),
            input.ident.span(),
        ))
    }
}

fn derive_object_or_error(input: DeriveInput) -> Result<proc_macro2::TokenStream, MacroError> {
    is_c_repr(&input, "#[derive(Object)]")?;

    let name = &input.ident;
    let parent = &get_fields(&input, "#[derive(Object)]")?[0].ident;

    Ok(quote! {
        ::qemu_api::assert_field_type!(#name, #parent,
            ::qemu_api::qom::ParentField<<#name as ::qemu_api::qom::ObjectImpl>::ParentType>);

        ::qemu_api::module_init! {
            MODULE_INIT_QOM => unsafe {
                ::qemu_api::bindings::type_register_static(&<#name as ::qemu_api::qom::ObjectImpl>::TYPE_INFO);
            }
        }
    })
}

#[proc_macro_derive(Object)]
pub fn derive_object(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let expanded = derive_object_or_error(input).unwrap_or_else(Into::into);

    TokenStream::from(expanded)
}

#[rustfmt::skip::macros(quote)]
fn derive_offsets_or_error(input: DeriveInput) -> Result<proc_macro2::TokenStream, MacroError> {
    is_c_repr(&input, "#[derive(offsets)]")?;

    let name = &input.ident;
    let fields = get_fields(&input, "#[derive(offsets)]")?;
    let field_names: Vec<&Ident> = fields.iter().map(|f| f.ident.as_ref().unwrap()).collect();
    let field_types: Vec<&Type> = fields.iter().map(|f| &f.ty).collect();
    let field_vis: Vec<&Visibility> = fields.iter().map(|f| &f.vis).collect();

    Ok(quote! {
	::qemu_api::with_offsets! {
	    struct #name {
		#(#field_vis #field_names: #field_types,)*
	    }
	}
    })
}

#[proc_macro_derive(offsets)]
pub fn derive_offsets(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let expanded = derive_offsets_or_error(input).unwrap_or_else(Into::into);

    TokenStream::from(expanded)
}
