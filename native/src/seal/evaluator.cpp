// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "seal/evaluator.h"
#include "seal/util/common.h"
#include "seal/util/galois.h"
#include "seal/util/iterator.h"
#include "seal/util/numth.h"
#include "seal/util/polyarithsmallmod.h"
#include "seal/util/polycore.h"
#include "seal/util/scalingvariant.h"
#include "seal/util/uintarith.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

using namespace std;
using namespace seal::util;

namespace seal
{
    namespace
    {
        template <typename T, typename S>
        inline bool are_same_scale(const T &value1, const S &value2) noexcept
        {
            return util::are_close<double>(value1.scale(), value2.scale());
        }
    } // namespace

    Evaluator::Evaluator(shared_ptr<SEALContext> context) : context_(move(context))
    {
        // Verify parameters
        if (!context_)
        {
            throw invalid_argument("invalid context");
        }
        if (!context_->parameters_set())
        {
            throw invalid_argument("encryption parameters are not set correctly");
        }

        // Calculate map from Zmstar to generator representation
        populate_Zmstar_to_generator();
    }

    void Evaluator::populate_Zmstar_to_generator()
    {
        uint64_t n = static_cast<uint64_t>(context_->first_context_data()->parms().poly_modulus_degree());
        uint64_t m = n << 1;

        for (uint64_t i = 0; i < n / 2; i++)
        {
            uint64_t galois_elt = exponentiate_uint64(3, i) & (m - 1);
            pair<uint64_t, uint64_t> temp_pair1{ i, 0 };
            Zmstar_to_generator_.emplace(galois_elt, temp_pair1);
            galois_elt = (exponentiate_uint64(3, i) * (m - 1)) & (m - 1);
            pair<uint64_t, uint64_t> temp_pair2{ i, 1 };
            Zmstar_to_generator_.emplace(galois_elt, temp_pair2);
        }
    }

    void Evaluator::negate_inplace(Ciphertext &encrypted)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted.parms_id());
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t encrypted_size = encrypted.size();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();

        // Negate each poly in the array
        for_each_n(PolyIterator(encrypted), encrypted_size, [&](auto I) {
            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const SmallModulus *>>(I, coeff_modulus),
                coeff_modulus_count,
                [&](auto J) { negate_poly_coeffmod(get<0>(J), coeff_count, *get<1>(J), get<0>(J)); });
        });
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::add_inplace(Ciphertext &encrypted1, const Ciphertext &encrypted2)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted1, context_) || !is_buffer_valid(encrypted1))
        {
            throw invalid_argument("encrypted1 is not valid for encryption parameters");
        }
        if (!is_metadata_valid_for(encrypted2, context_) || !is_buffer_valid(encrypted2))
        {
            throw invalid_argument("encrypted2 is not valid for encryption parameters");
        }
        if (encrypted1.parms_id() != encrypted2.parms_id())
        {
            throw invalid_argument("encrypted1 and encrypted2 parameter mismatch");
        }
        if (encrypted1.is_ntt_form() != encrypted2.is_ntt_form())
        {
            throw invalid_argument("NTT form mismatch");
        }
        if (!are_same_scale(encrypted1, encrypted2))
        {
            throw invalid_argument("scale mismatch");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted1.parms_id());
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();
        size_t encrypted1_size = encrypted1.size();
        size_t encrypted2_size = encrypted2.size();
        size_t max_count = max(encrypted1_size, encrypted2_size);
        size_t min_count = min(encrypted1_size, encrypted2_size);

        // Size check
        if (!product_fits_in(max_count, coeff_count))
        {
            throw logic_error("invalid parameters");
        }

        // Prepare destination
        encrypted1.resize(context_, context_data.parms_id(), max_count);

        // Add ciphertexts
        for_each_n(IteratorTuple<PolyIterator, ConstPolyIterator>(encrypted1, encrypted2), min_count, [&](auto I) {
            for_each_n(
                IteratorTuple<decltype(I), IteratorWrapper<const SmallModulus *>>(I, coeff_modulus),
                coeff_modulus_count, [&](auto J) {
                    add_poly_poly_coeffmod(
                        get<0>(get<0>(J)), get<1>(get<0>(J)), coeff_count, *get<1>(J), get<0>(get<0>(J)));
                });
        });

        // Copy the remainding polys of the array with larger count into encrypted1
        if (encrypted1_size < encrypted2_size)
        {
            set_poly_poly(
                encrypted2.data(min_count), coeff_count * (encrypted2_size - encrypted1_size), coeff_modulus_count,
                encrypted1.data(encrypted1_size));
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted1.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::add_many(const vector<Ciphertext> &encrypteds, Ciphertext &destination)
    {
        if (encrypteds.empty())
        {
            throw invalid_argument("encrypteds cannot be empty");
        }
        for (size_t i = 0; i < encrypteds.size(); i++)
        {
            if (&encrypteds[i] == &destination)
            {
                throw invalid_argument("encrypteds must be different from destination");
            }
        }
        destination = encrypteds[0];
        for (size_t i = 1; i < encrypteds.size(); i++)
        {
            add_inplace(destination, encrypteds[i]);
        }
    }

    void Evaluator::sub_inplace(Ciphertext &encrypted1, const Ciphertext &encrypted2)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted1, context_) || !is_buffer_valid(encrypted1))
        {
            throw invalid_argument("encrypted1 is not valid for encryption parameters");
        }
        if (!is_metadata_valid_for(encrypted2, context_) || !is_buffer_valid(encrypted2))
        {
            throw invalid_argument("encrypted2 is not valid for encryption parameters");
        }
        if (encrypted1.parms_id() != encrypted2.parms_id())
        {
            throw invalid_argument("encrypted1 and encrypted2 parameter mismatch");
        }
        if (encrypted1.is_ntt_form() != encrypted2.is_ntt_form())
        {
            throw invalid_argument("NTT form mismatch");
        }
        if (!are_same_scale(encrypted1, encrypted2))
        {
            throw invalid_argument("scale mismatch");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted1.parms_id());
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();
        size_t encrypted1_size = encrypted1.size();
        size_t encrypted2_size = encrypted2.size();
        size_t max_count = max(encrypted1_size, encrypted2_size);
        size_t min_count = min(encrypted1_size, encrypted2_size);

        // Size check
        if (!product_fits_in(max_count, coeff_count))
        {
            throw logic_error("invalid parameters");
        }

        // Prepare destination
        encrypted1.resize(context_, context_data.parms_id(), max_count);

        // Set up iterators in the function scope
        PolyIterator encrypted1_iter(encrypted1);
        ConstPolyIterator encrypted2_iter(encrypted2);

        // Subtract polynomials
        for_each_n(
            IteratorTuple<PolyIterator, ConstPolyIterator>(encrypted1_iter, encrypted2_iter), min_count, [&](auto I) {
                for_each_n(
                    IteratorTuple<decltype(I), IteratorWrapper<const SmallModulus *>>(I, coeff_modulus),
                    coeff_modulus_count, [&](auto J) {
                        sub_poly_poly_coeffmod(
                            get<0>(get<0>(J)), get<1>(get<0>(J)), coeff_count, *get<1>(J), get<0>(get<0>(J)));
                    });
            });

        // If encrypted2 has larger count, negate remaining entries
        if (encrypted1_size < encrypted2_size)
        {
            // Advance encrypted1_iter and encrypted2_iter to min_count (i.e., encrypted1_size)
            advance(encrypted1_iter, min_count);
            advance(encrypted2_iter, min_count);

            for_each_n(
                IteratorTuple<PolyIterator, ConstPolyIterator>(encrypted1_iter, encrypted2_iter),
                encrypted2_size - min_count, [&](auto I) {
                    for_each_n(
                        IteratorTuple<decltype(I), IteratorWrapper<const SmallModulus *>>(I, coeff_modulus),
                        coeff_modulus_count, [&](auto J) {
                            negate_poly_coeffmod(get<1>(get<0>(J)), coeff_count, *get<1>(J), get<0>(get<0>(J)));
                        });
                });
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted1.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::multiply_inplace(Ciphertext &encrypted1, const Ciphertext &encrypted2, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted1, context_) || !is_buffer_valid(encrypted1))
        {
            throw invalid_argument("encrypted1 is not valid for encryption parameters");
        }
        if (!is_metadata_valid_for(encrypted2, context_) || !is_buffer_valid(encrypted2))
        {
            throw invalid_argument("encrypted2 is not valid for encryption parameters");
        }
        if (encrypted1.parms_id() != encrypted2.parms_id())
        {
            throw invalid_argument("encrypted1 and encrypted2 parameter mismatch");
        }

        auto context_data_ptr = context_->first_context_data();
        switch (context_data_ptr->parms().scheme())
        {
        case scheme_type::BFV:
            bfv_multiply(encrypted1, encrypted2, pool);
            break;

        case scheme_type::CKKS:
            ckks_multiply(encrypted1, encrypted2, pool);
            break;

        default:
            throw invalid_argument("unsupported scheme");
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted1.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::bfv_multiply(Ciphertext &encrypted1, const Ciphertext &encrypted2, MemoryPoolHandle pool)
    {
        if (encrypted1.is_ntt_form() || encrypted2.is_ntt_form())
        {
            throw invalid_argument("encrypted1 or encrypted2 cannot be in NTT form");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted1.parms_id());
        auto &parms = context_data.parms();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t base_q_size = parms.coeff_modulus().size();
        size_t encrypted1_size = encrypted1.size();
        size_t encrypted2_size = encrypted2.size();

        uint64_t plain_modulus = parms.plain_modulus().value();
        auto rns_tool = context_data.rns_tool();
        size_t base_Bsk_size = rns_tool->base_Bsk()->size();
        size_t base_Bsk_m_tilde_size = rns_tool->base_Bsk_m_tilde()->size();

        // Determine destination.size()
        size_t dest_size = sub_safe(add_safe(encrypted1_size, encrypted2_size), size_t(1));

        // Size check
        if (!product_fits_in(dest_size, coeff_count, base_Bsk_m_tilde_size))
        {
            throw logic_error("invalid parameters");
        }

        // Set up iterators for bases
        IteratorWrapper<const SmallModulus *> base_q_iter(parms.coeff_modulus());
        IteratorWrapper<const SmallModulus *> base_Bsk_iter(rns_tool->base_Bsk()->base());

        // Set up iterators for NTT tables
        IteratorWrapper<const SmallNTTTables *> base_q_ntt_tables_iter(context_data.small_ntt_tables());
        IteratorWrapper<const SmallNTTTables *> base_Bsk_ntt_tables_iter(rns_tool->base_Bsk_small_ntt_tables());

        // Microsoft SEAL uses BEHZ-style RNS multiplication. This process is somewhat complex and consists of the
        // following steps:
        //
        // (1) Lift encrypted1 and encrypted2 (initially in base q) to an extended base q U Bsk U {m_tilde}
        // (2) Remove extra multiples of q from the results with Montgomery reduction, switching base to q U Bsk
        // (3) Transform the data to NTT form
        // (4) Compute the ciphertext polynomial product using dyadic multiplication
        // (5) Transform the data back from NTT form
        // (6) Multiply the result by t (plain_modulus)
        // (7) Scale the result by q using a divide-and-floor algorithm, switching base to Bsk
        // (8) Use Shenoy-Kumaresan method to convert the result to base q

        // Resize encrypted1 to destination size
        encrypted1.resize(context_, context_data.parms_id(), dest_size);

        // Set up iterators for input ciphertexts
        PolyIterator encrypted1_iter(encrypted1);
        ConstPolyIterator encrypted2_iter(encrypted2);

        // This lambda function takes as input an iterator_tuple with three components:
        //
        // 1. ConstRNSIterator to read an input polynomial from
        // 2. RNSIterator for the output in base q
        // 3. RNSIterator for the output in base Bsk
        //
        // It performs steps (1)-(3) of the BEHZ multiplication (see above) on the given input polynomial (given as an
        // RNSIterator or ConstRNSIterator) and writes the results in base q and base Bsk to the given output
        // iterators.
        auto behz_extend_base_convert_to_ntt = [&](auto I) {
            // Make copy of input polynomial (in base q) and convert to NTT form
            for_each_n(
                IteratorTuple<ConstRNSIterator, RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                    get<0>(I), get<1>(I), base_q_ntt_tables_iter),
                base_q_size, [&](auto J) {
                    // First copy to output
                    set_uint_uint(get<0>(J), coeff_count, get<1>(J));

                    // Transform to NTT form in base q
                    // Lazy reduction
                    ntt_negacyclic_harvey_lazy(get<1>(J), *get<2>(J));
                });

            // Allocate temporary space for a polynomial in the Bsk U {m_tilde} base
            auto temp(allocate_poly(coeff_count, base_Bsk_m_tilde_size, pool));

            // (1) Convert from base q to base Bsk U {m_tilde}
            rns_tool->fastbconv_m_tilde(get<0>(I), temp.get(), pool);

            // (2) Reduce q-overflows in with Montgomery reduction, switching base to Bsk
            rns_tool->sm_mrq(temp.get(), get<2>(I), pool);

            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                    get<2>(I), base_Bsk_ntt_tables_iter),
                base_Bsk_size, [&](auto J) {
                    // Transform to NTT form in base Bsk
                    // Lazy reduction
                    ntt_negacyclic_harvey_lazy(get<0>(J), *get<1>(J));
                });
        };

        // Allocate space for a base q output of behz_extend_base_convert_to_ntt for encrypted1
        auto encrypted1_q(allocate_poly(coeff_count * encrypted1_size, base_q_size, pool));
        PolyIterator encrypted1_q_iter(encrypted1_q.get(), coeff_count, base_q_size);

        // Allocate space for a base Bsk output of behz_extend_base_convert_to_ntt for encrypted1
        auto encrypted1_Bsk(allocate_poly(coeff_count * encrypted1_size, base_Bsk_size, pool));
        PolyIterator encrypted1_Bsk_iter(encrypted1_Bsk.get(), coeff_count, base_Bsk_size);

        // Perform BEHZ steps (1)-(3) for encrypted1
        for_each_n(
            IteratorTuple<ConstPolyIterator, PolyIterator, PolyIterator>(
                encrypted1_iter, encrypted1_q_iter, encrypted1_Bsk_iter),
            encrypted1_size, behz_extend_base_convert_to_ntt);

        // Repeat for encrypted2
        auto encrypted2_q(allocate_poly(coeff_count * encrypted2_size, base_q_size, pool));
        PolyIterator encrypted2_q_iter(encrypted2_q.get(), coeff_count, base_q_size);

        auto encrypted2_Bsk(allocate_poly(coeff_count * encrypted2_size, base_Bsk_size, pool));
        PolyIterator encrypted2_Bsk_iter(encrypted2_Bsk.get(), coeff_count, base_Bsk_size);

        for_each_n(
            IteratorTuple<ConstPolyIterator, PolyIterator, PolyIterator>(
                encrypted2_iter, encrypted2_q_iter, encrypted2_Bsk_iter),
            encrypted2_size, behz_extend_base_convert_to_ntt);

        // Allocate temporary space for the output of step (4)
        // We allocate space separately for the base q and the base Bsk components
        auto temp_dest_q(allocate_zero_poly(coeff_count * dest_size, base_q_size, pool));
        PolyIterator temp_dest_q_iter(temp_dest_q.get(), coeff_count, base_q_size);

        auto temp_dest_Bsk(allocate_zero_poly(coeff_count * dest_size, base_Bsk_size, pool));
        PolyIterator temp_dest_Bsk_iter(temp_dest_Bsk.get(), coeff_count, base_Bsk_size);

        // Perform BEHZ step (4): dyadic multiplication on arbitrary size ciphertexts
        for (size_t secret_power_index = 0; secret_power_index < dest_size; secret_power_index++)
        {
            // We iterate over relevant components of encrypted1 and encrypted2 in increasing order for
            // encrypted1 and reversed (decreasing) order for encrypted2. The bounds for the indices of
            // the relevant terms are obtained as follows.
            size_t curr_encrypted1_last = min(secret_power_index, encrypted1_size - 1);
            size_t curr_encrypted2_first = min(secret_power_index, encrypted2_size - 1);
            size_t curr_encrypted1_first = secret_power_index - curr_encrypted2_first;
            // size_t curr_encrypted2_last = secret_power_index - curr_encrypted1_last;

            // The total number of dyadic products is now easy to compute
            size_t steps = curr_encrypted1_last - curr_encrypted1_first + 1;

            // This lambda function computes the ciphertext product for BFV multiplication. Since we use the BEHZ
            // approach, the multiplication of individual polynomials is done using a dyadic product where the inputs
            // are already in NTT form. The arguments of the lambda function are expected to be as follows:
            //
            // 1. a PolyIterator pointing to the beginning of the first input ciphertext (in NTT form)
            // 2. a PolyIterator pointing to the beginning of the second input ciphertext (in NTT form)
            // 3. an IteratorWrapper pointing to an array of SmallModulus elements for the base
            // 4. the size of the base
            // 5. a PolyIterator pointing to the beginning of the output ciphertext
            auto behz_ciphertext_product = [&](auto in1_iter, auto in2_iter, auto base_iter, size_t base_size,
                                               auto out_iter) {
                // Create a shifted iterator for the first input
                auto shifted_in1_iter = in1_iter;
                advance(shifted_in1_iter, curr_encrypted1_first);

                // Create a shifted reverse iterator for the second input
                auto shifted_in2_iter = in2_iter;
                advance(shifted_in2_iter, curr_encrypted2_first);
                auto shifted_reversed_in2_iter = ReverseIterator<PolyIterator>(shifted_in2_iter);

                // Create a shifted iterator for the output
                auto shifted_out_iter = out_iter;
                advance(shifted_out_iter, secret_power_index);

                for_each_n(
                    IteratorTuple<PolyIterator, ReverseIterator<PolyIterator>>(
                        shifted_in1_iter, shifted_reversed_in2_iter),
                    steps, [&](auto I) {
                        // Extra care needed here: shifted_out_iter must be dereferenced once to
                        // produce an appropriate RNSIterator.
                        for_each_n(
                            IteratorTuple<decltype(I), IteratorWrapper<const SmallModulus *>, RNSIterator>(
                                I, base_iter, *shifted_out_iter),
                            base_size, [&](auto J) {
                                auto temp(allocate_uint(coeff_count, pool));
                                dyadic_product_coeffmod(
                                    get<0>(get<0>(J)), get<1>(get<0>(J)), coeff_count, *get<1>(J), temp.get());
                                add_poly_poly_coeffmod(temp.get(), get<2>(J), coeff_count, *get<1>(J), get<2>(J));
                            });
                    });
            };

            // Perform the BEHZ ciphertext product both for base q and base Bsk
            behz_ciphertext_product(encrypted1_q_iter, encrypted2_q_iter, base_q_iter, base_q_size, temp_dest_q_iter);
            behz_ciphertext_product(
                encrypted1_Bsk_iter, encrypted2_Bsk_iter, base_Bsk_iter, base_Bsk_size, temp_dest_Bsk_iter);
        }

        // Perform BEHZ step (5): transform data from NTT form
        for_each_n(
            IteratorTuple<PolyIterator, PolyIterator>(temp_dest_q_iter, temp_dest_Bsk_iter), dest_size, [&](auto I) {
                for_each_n(
                    IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                        get<0>(I), base_q_ntt_tables_iter),
                    base_q_size, [&](auto J) { inverse_ntt_negacyclic_harvey(get<0>(J), *get<1>(J)); });

                for_each_n(
                    IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                        get<1>(I), base_Bsk_ntt_tables_iter),
                    base_Bsk_size, [&](auto J) { inverse_ntt_negacyclic_harvey(get<0>(J), *get<1>(J)); });
            });

        // Perform BEHZ steps (6)-(8)
        for_each_n(
            IteratorTuple<PolyIterator, PolyIterator, PolyIterator>(
                temp_dest_q_iter, temp_dest_Bsk_iter, encrypted1_iter),
            dest_size, [&](auto I) {
                // Bring together the base q and base Bsk components into a single allocation
                auto temp_q_Bsk(allocate_poly(coeff_count, base_q_size + base_Bsk_size, pool));
                RNSIterator temp_q_Bsk_iter(temp_q_Bsk.get(), coeff_count);

                // Step (6): multiply base q components by t (plain_modulus)
                for_each_n(
                    IteratorTuple<ConstRNSIterator, RNSIterator, IteratorWrapper<const SmallModulus *>>(
                        get<0>(I), temp_q_Bsk_iter, base_q_iter),
                    base_q_size, [&](auto J) {
                        multiply_poly_scalar_coeffmod(get<0>(J), coeff_count, plain_modulus, *get<2>(J), get<1>(J));
                    });

                // Advance to the base Bsk part in temp and multiply base Bsk components by t
                advance(temp_q_Bsk_iter, base_q_size);
                for_each_n(
                    IteratorTuple<ConstRNSIterator, RNSIterator, IteratorWrapper<const SmallModulus *>>(
                        get<1>(I), temp_q_Bsk_iter, base_Bsk_iter),
                    base_Bsk_size, [&](auto J) {
                        multiply_poly_scalar_coeffmod(get<0>(J), coeff_count, plain_modulus, *get<2>(J), get<1>(J));
                    });

                // Allocate yet another temporary for fast divide-and-floor result in base Bsk
                auto temp_Bsk(allocate_poly(coeff_count, base_Bsk_size, pool));

                // Step (7): divide by q and floor, producing a result in base Bsk
                rns_tool->fast_floor(temp_q_Bsk.get(), temp_Bsk.get(), pool);

                // Step (8): use Shenoy-Kumaresan method to convert the result to base q and write to encrypted1
                rns_tool->fastbconv_sk(temp_Bsk.get(), get<2>(I), pool);
            });
    }

    void Evaluator::ckks_multiply(Ciphertext &encrypted1, const Ciphertext &encrypted2, MemoryPoolHandle pool)
    {
        if (!(encrypted1.is_ntt_form() && encrypted2.is_ntt_form()))
        {
            throw invalid_argument("encrypted1 or encrypted2 must be in NTT form");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted1.parms_id());
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();
        size_t encrypted1_size = encrypted1.size();
        size_t encrypted2_size = encrypted2.size();

        double new_scale = encrypted1.scale() * encrypted2.scale();

        // Check that scale is positive and not too large
        if (new_scale <= 0 || (static_cast<int>(log2(new_scale)) >= context_data.total_coeff_modulus_bit_count()))
        {
            throw invalid_argument("scale out of bounds");
        }

        // Determine destination.size()
        // Default is 3 (c_0, c_1, c_2)
        size_t dest_size = sub_safe(add_safe(encrypted1_size, encrypted2_size), size_t(1));

        // Size check
        if (!product_fits_in(dest_size, coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        // Set up iterator for the base
        IteratorWrapper<const SmallModulus *> coeff_modulus_iter(parms.coeff_modulus());

        // Prepare destination
        encrypted1.resize(context_, context_data.parms_id(), dest_size);

        // Set up iterators for input ciphertexts
        PolyIterator encrypted1_iter(encrypted1);
        ConstPolyIterator encrypted2_iter(encrypted2);

        // Allocate temporary space for the result
        auto temp(allocate_zero_poly(coeff_count * dest_size, coeff_modulus_count, pool));
        PolyIterator temp_iter(temp.get(), coeff_count, coeff_modulus_count);

        for (size_t secret_power_index = 0; secret_power_index < dest_size; secret_power_index++, ++temp_iter)
        {
            // We iterate over relevant components of encrypted1 and encrypted2 in increasing order for
            // encrypted1 and reversed (decreasing) order for encrypted2. The bounds for the indices of
            // the relevant terms are obtained as follows.
            size_t curr_encrypted1_last = min(secret_power_index, encrypted1_size - 1);
            size_t curr_encrypted2_first = min(secret_power_index, encrypted2_size - 1);
            size_t curr_encrypted1_first = secret_power_index - curr_encrypted2_first;
            // size_t curr_encrypted2_last = secret_power_index - curr_encrypted1_last;

            // The total number of dyadic products is now easy to compute
            size_t steps = curr_encrypted1_last - curr_encrypted1_first + 1;

            // Create a shifted iterator for the first input
            auto shifted_encrypted1_iter = encrypted1_iter;
            advance(shifted_encrypted1_iter, curr_encrypted1_first);

            // Create a shifted reverse iterator for the second input
            auto shifted_encrypted2_iter = encrypted2_iter;
            advance(shifted_encrypted2_iter, curr_encrypted2_first);
            auto shifted_reversed_encrypted2_iter = ReverseIterator<ConstPolyIterator>(shifted_encrypted2_iter);

            for_each_n(
                IteratorTuple<PolyIterator, ReverseIterator<ConstPolyIterator>>(
                    shifted_encrypted1_iter, shifted_reversed_encrypted2_iter),
                steps, [&](auto I) {
                    // Extra care needed here:
                    // temp_iter must be dereferenced once to produce an appropriate RNSIterator
                    for_each_n(
                        IteratorTuple<decltype(I), IteratorWrapper<const SmallModulus *>, RNSIterator>(
                            I, coeff_modulus_iter, *temp_iter),
                        coeff_modulus_count, [&](auto J) {
                            auto temp(allocate_uint(coeff_count, pool));
                            dyadic_product_coeffmod(
                                get<0>(get<0>(J)), get<1>(get<0>(J)), coeff_count, *get<1>(J), temp.get());
                            add_poly_poly_coeffmod(temp.get(), get<2>(J), coeff_count, *get<1>(J), get<2>(J));
                        });
                });
        }

        // Set the final result
        set_poly_poly(temp.get(), coeff_count * dest_size, coeff_modulus_count, encrypted1.data());

        // Set the scale
        encrypted1.scale() = new_scale;
    }

    void Evaluator::square_inplace(Ciphertext &encrypted, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }

        auto context_data_ptr = context_->first_context_data();
        switch (context_data_ptr->parms().scheme())
        {
        case scheme_type::BFV:
            bfv_square(encrypted, move(pool));
            break;

        case scheme_type::CKKS:
            ckks_square(encrypted, move(pool));
            break;

        default:
            throw invalid_argument("unsupported scheme");
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::bfv_square(Ciphertext &encrypted, MemoryPoolHandle pool)
    {
        if (encrypted.is_ntt_form())
        {
            throw invalid_argument("encrypted cannot be in NTT form");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted.parms_id());
        auto &parms = context_data.parms();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t base_q_size = parms.coeff_modulus().size();
        size_t encrypted_size = encrypted.size();

        uint64_t plain_modulus = parms.plain_modulus().value();
        auto rns_tool = context_data.rns_tool();
        size_t base_Bsk_size = rns_tool->base_Bsk()->size();
        size_t base_Bsk_m_tilde_size = rns_tool->base_Bsk_m_tilde()->size();

        // Optimization implemented currently only for size 2 ciphertexts
        if (encrypted_size != 2)
        {
            bfv_multiply(encrypted, encrypted, move(pool));
            return;
        }

        // Determine destination.size()
        size_t dest_size = sub_safe(add_safe(encrypted_size, encrypted_size), size_t(1));

        // Size check
        if (!product_fits_in(dest_size, coeff_count, base_Bsk_m_tilde_size))
        {
            throw logic_error("invalid parameters");
        }

        // Set up iterators for bases
        IteratorWrapper<const SmallModulus *> base_q_iter(parms.coeff_modulus());
        IteratorWrapper<const SmallModulus *> base_Bsk_iter(rns_tool->base_Bsk()->base());

        // Set up iterators for NTT tables
        IteratorWrapper<const SmallNTTTables *> base_q_ntt_tables_iter(context_data.small_ntt_tables());
        IteratorWrapper<const SmallNTTTables *> base_Bsk_ntt_tables_iter(rns_tool->base_Bsk_small_ntt_tables());

        // Microsoft SEAL uses BEHZ-style RNS multiplication. For details, see Evaluator::bfv_multiply. This function
        // uses additionally Karatsuba multiplication to reduce the complexity of squaring a size-2 ciphertext, but the
        // steps are otherwise the same as in Evaluator::bfv_multiply.

        // Resize encrypted to destination size
        encrypted.resize(context_, context_data.parms_id(), dest_size);

        // Set up iterators for input ciphertexts
        PolyIterator encrypted_iter(encrypted);

        // This lambda function takes as input an iterator_tuple with three components:
        //
        // 1. ConstRNSIterator to read an input polynomial from
        // 2. RNSIterator for the output in base q
        // 3. RNSIterator for the output in base Bsk
        //
        // It performs steps (1)-(3) of the BEHZ multiplication on the given input polynomial (given as an RNSIterator
        // or ConstRNSIterator) and writes the results in base q and base Bsk to the given output iterators.
        auto behz_extend_base_convert_to_ntt = [&](auto I) {
            // Make copy of input polynomial (in base q) and convert to NTT form
            for_each_n(
                IteratorTuple<ConstRNSIterator, RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                    get<0>(I), get<1>(I), base_q_ntt_tables_iter),
                base_q_size, [&](auto J) {
                    // First copy to output
                    set_uint_uint(get<0>(J), coeff_count, get<1>(J));

                    // Transform to NTT form in base q
                    // Lazy reduction
                    ntt_negacyclic_harvey_lazy(get<1>(J), *get<2>(J));
                });

            // Allocate temporary space for a polynomial in the Bsk U {m_tilde} base
            auto temp(allocate_poly(coeff_count, base_Bsk_m_tilde_size, pool));

            // (1) Convert from base q to base Bsk U {m_tilde}
            rns_tool->fastbconv_m_tilde(get<0>(I), temp.get(), pool);

            // (2) Reduce q-overflows in with Montgomery reduction, switching base to Bsk
            rns_tool->sm_mrq(temp.get(), get<2>(I), pool);

            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                    get<2>(I), base_Bsk_ntt_tables_iter),
                base_Bsk_size, [&](auto J) {
                    // Transform to NTT form in base Bsk
                    // Lazy reduction
                    ntt_negacyclic_harvey_lazy(get<0>(J), *get<1>(J));
                });
        };

        // Allocate space for a base q output of behz_extend_base_convert_to_ntt
        auto encrypted_q(allocate_poly(coeff_count * encrypted_size, base_q_size, pool));
        PolyIterator encrypted_q_iter(encrypted_q.get(), coeff_count, base_q_size);

        // Allocate space for a base Bsk output of behz_extend_base_convert_to_ntt
        auto encrypted_Bsk(allocate_poly(coeff_count * encrypted_size, base_Bsk_size, pool));
        PolyIterator encrypted_Bsk_iter(encrypted_Bsk.get(), coeff_count, base_Bsk_size);

        // Perform BEHZ steps (1)-(3)
        for_each_n(
            IteratorTuple<ConstPolyIterator, PolyIterator, PolyIterator>(
                encrypted_iter, encrypted_q_iter, encrypted_Bsk_iter),
            encrypted_size, behz_extend_base_convert_to_ntt);

        // Allocate temporary space for the output of step (4)
        // We allocate space separately for the base q and the base Bsk components
        auto temp_dest_q(allocate_zero_poly(coeff_count * dest_size, base_q_size, pool));
        PolyIterator temp_dest_q_iter(temp_dest_q.get(), coeff_count, base_q_size);

        auto temp_dest_Bsk(allocate_zero_poly(coeff_count * dest_size, base_Bsk_size, pool));
        PolyIterator temp_dest_Bsk_iter(temp_dest_Bsk.get(), coeff_count, base_Bsk_size);

        // Perform BEHZ step (4): dyadic Karatsuba-squaring on size-2 ciphertexts

        // This lambda function computes the size-2 ciphertext square for BFV multiplication. Since we use the BEHZ
        // approach, the multiplication of individual polynomials is done using a dyadic product where the inputs
        // are already in NTT form. The arguments of the lambda function are expected to be as follows:
        //
        // 1. a PolyIterator pointing to the beginning of the input ciphertext (in NTT form)
        // 3. an IteratorWrapper pointing to an array of SmallModulus elements for the base
        // 4. the size of the base
        // 5. a PolyIterator pointing to the beginning of the output ciphertext
        auto behz_ciphertext_square = [&](auto in_iter, auto base_iter, size_t base_size, auto out_iter) {
            // Make a copy of the input iterator
            auto in_iter_copy = in_iter;

            // Compute c0^2
            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const SmallModulus *>, RNSIterator>(
                    *in_iter, base_iter, *out_iter),
                base_size,
                [&](auto I) { dyadic_product_coeffmod(get<0>(I), get<0>(I), coeff_count, *get<1>(I), get<2>(I)); });

            // Advance in_iter and out_iter
            ++in_iter;
            ++out_iter;

            // Compute 2*c0*c1
            for_each_n(
                IteratorTuple<RNSIterator, RNSIterator, IteratorWrapper<const SmallModulus *>, RNSIterator>(
                    *in_iter, *in_iter_copy, base_iter, *out_iter),
                base_size, [&](auto I) {
                    dyadic_product_coeffmod(get<0>(I), get<1>(I), coeff_count, *get<2>(I), get<3>(I));
                    add_poly_poly_coeffmod(get<3>(I), get<3>(I), coeff_count, *get<2>(I), get<3>(I));
                });

            // Advance out_iter manually
            ++out_iter;

            // Compute c1^2
            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const SmallModulus *>, RNSIterator>(
                    *in_iter, base_iter, *out_iter),
                base_size,
                [&](auto I) { dyadic_product_coeffmod(get<0>(I), get<0>(I), coeff_count, *get<1>(I), get<2>(I)); });
        };

        // Perform the BEHZ ciphertext square both for base q and base Bsk
        behz_ciphertext_square(encrypted_q_iter, base_q_iter, base_q_size, temp_dest_q_iter);
        behz_ciphertext_square(encrypted_Bsk_iter, base_Bsk_iter, base_Bsk_size, temp_dest_Bsk_iter);

        // Perform BEHZ step (5): transform data from NTT form
        for_each_n(
            IteratorTuple<PolyIterator, PolyIterator>(temp_dest_q_iter, temp_dest_Bsk_iter), dest_size, [&](auto I) {
                for_each_n(
                    IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                        get<0>(I), base_q_ntt_tables_iter),
                    base_q_size, [&](auto J) { inverse_ntt_negacyclic_harvey(get<0>(J), *get<1>(J)); });

                for_each_n(
                    IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(
                        get<1>(I), base_Bsk_ntt_tables_iter),
                    base_Bsk_size, [&](auto J) { inverse_ntt_negacyclic_harvey(get<0>(J), *get<1>(J)); });
            });

        // Perform BEHZ steps (6)-(8)
        for_each_n(
            IteratorTuple<PolyIterator, PolyIterator, PolyIterator>(
                temp_dest_q_iter, temp_dest_Bsk_iter, encrypted_iter),
            dest_size, [&](auto I) {
                // Bring together the base q and base Bsk components into a single allocation
                auto temp_q_Bsk(allocate_poly(coeff_count, base_q_size + base_Bsk_size, pool));
                RNSIterator temp_q_Bsk_iter(temp_q_Bsk.get(), coeff_count);

                // Step (6): multiply base q components by t (plain_modulus)
                for_each_n(
                    IteratorTuple<RNSIterator, RNSIterator, IteratorWrapper<const SmallModulus *>>(
                        get<0>(I), temp_q_Bsk_iter, base_q_iter),
                    base_q_size, [&](auto J) {
                        multiply_poly_scalar_coeffmod(get<0>(J), coeff_count, plain_modulus, *get<2>(J), get<1>(J));
                    });

                // Advance to the base Bsk part in temp and multiply base Bsk components by t
                advance(temp_q_Bsk_iter, base_q_size);
                for_each_n(
                    IteratorTuple<RNSIterator, RNSIterator, IteratorWrapper<const SmallModulus *>>(
                        get<1>(I), temp_q_Bsk_iter, base_Bsk_iter),
                    base_Bsk_size, [&](auto J) {
                        multiply_poly_scalar_coeffmod(get<0>(J), coeff_count, plain_modulus, *get<2>(J), get<1>(J));
                    });

                // Allocate yet another temporary for fast divide-and-floor result in base Bsk
                auto temp_Bsk(allocate_poly(coeff_count, base_Bsk_size, pool));

                // Step (7): divide by q and floor, producing a result in base Bsk
                rns_tool->fast_floor(temp_q_Bsk.get(), temp_Bsk.get(), pool);

                // Step (8): use Shenoy-Kumaresan method to convert the result to base q and write to encrypted1
                rns_tool->fastbconv_sk(temp_Bsk.get(), get<2>(I), pool);
            });
    }

    void Evaluator::ckks_square(Ciphertext &encrypted, MemoryPoolHandle pool)
    {
        if (!encrypted.is_ntt_form())
        {
            throw invalid_argument("encrypted must be in NTT form");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted.parms_id());
        auto &parms = context_data.parms();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = parms.coeff_modulus().size();
        size_t encrypted_size = encrypted.size();

        // Optimization implemented currently only for size 2 ciphertexts
        if (encrypted_size != 2)
        {
            ckks_multiply(encrypted, encrypted, move(pool));
            return;
        }

        double new_scale = encrypted.scale() * encrypted.scale();

        // Check that scale is positive and not too large
        if (new_scale <= 0 || (static_cast<int>(log2(new_scale)) >= context_data.total_coeff_modulus_bit_count()))
        {
            throw invalid_argument("scale out of bounds");
        }

        // Determine destination.size()
        // Default is 3 (c_0, c_1, c_2)
        size_t dest_size = sub_safe(add_safe(encrypted_size, encrypted_size), size_t(1));

        // Size check
        if (!product_fits_in(dest_size, coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        // Set up iterator for the base
        IteratorWrapper<const SmallModulus *> coeff_modulus_iter(parms.coeff_modulus());

        // Prepare destination
        encrypted.resize(context_, context_data.parms_id(), dest_size);

        // Set up iterators for input ciphertext
        PolyIterator encrypted_iter(encrypted);

        // Allocate temporary space for the result
        auto temp(allocate_zero_poly(coeff_count * dest_size, coeff_modulus_count, pool));
        PolyIterator temp_iter(temp.get(), coeff_count, coeff_modulus_count);

        // Make a copy of the input iterator
        auto encrypted_iter_copy = encrypted_iter;

        // Compute c0^2
        for_each_n(
            IteratorTuple<RNSIterator, IteratorWrapper<const SmallModulus *>, RNSIterator>(
                *encrypted_iter, coeff_modulus_iter, *temp_iter),
            coeff_modulus_count,
            [&](auto I) { dyadic_product_coeffmod(get<0>(I), get<0>(I), coeff_count, *get<1>(I), get<2>(I)); });

        // Advance encrypted_iter and temp_iter
        ++encrypted_iter;
        ++temp_iter;

        // Compute 2*c0*c1
        for_each_n(
            IteratorTuple<RNSIterator, RNSIterator, IteratorWrapper<const SmallModulus *>, RNSIterator>(
                *encrypted_iter, *encrypted_iter_copy, coeff_modulus_iter, *temp_iter),
            coeff_modulus_count, [&](auto I) {
                dyadic_product_coeffmod(get<0>(I), get<1>(I), coeff_count, *get<2>(I), get<3>(I));
                add_poly_poly_coeffmod(get<3>(I), get<3>(I), coeff_count, *get<2>(I), get<3>(I));
            });

        // Advance temp_iter manually
        ++temp_iter;

        // Compute c1^2
        for_each_n(
            IteratorTuple<RNSIterator, IteratorWrapper<const SmallModulus *>, RNSIterator>(
                *encrypted_iter, coeff_modulus_iter, *temp_iter),
            coeff_modulus_count,
            [&](auto I) { dyadic_product_coeffmod(get<0>(I), get<0>(I), coeff_count, *get<1>(I), get<2>(I)); });

        // Set the final result
        set_poly_poly(temp.get(), coeff_count * dest_size, coeff_modulus_count, encrypted.data());

        // Set the scale
        encrypted.scale() = new_scale;
    }

    void Evaluator::relinearize_internal(
        Ciphertext &encrypted, const RelinKeys &relin_keys, size_t destination_size, MemoryPoolHandle pool)
    {
        // Verify parameters.
        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (relin_keys.parms_id() != context_->key_parms_id())
        {
            throw invalid_argument("relin_keys is not valid for encryption parameters");
        }

        size_t encrypted_size = encrypted.size();

        // Verify parameters.
        if (destination_size < 2 || destination_size > encrypted_size)
        {
            throw invalid_argument("destination_size must be at least 2 and less than or equal to current count");
        }
        if (relin_keys.size() < sub_safe(encrypted_size, size_t(2)))
        {
            throw invalid_argument("not enough relinearization keys");
        }

        // If encrypted is already at the desired level, return
        if (destination_size == encrypted_size)
        {
            return;
        }

        // Calculate number of relinearize_one_step calls needed
        size_t relins_needed = encrypted_size - destination_size;

        // Iterator pointing to the last component of encrypted
        PolyIterator encrypted_iter(encrypted);
        advance(encrypted_iter, encrypted_size - 1);

        for (size_t i = 0; i < relins_needed; i++)
        {
            switch_key_inplace(
                encrypted, *encrypted_iter, static_cast<const KSwitchKeys &>(relin_keys),
                RelinKeys::get_index(encrypted_size - 1), pool);
            encrypted_size--;
        }

        // Put the output of final relinearization into destination.
        // Prepare destination only at this point because we are resizing down
        encrypted.resize(context_, context_data_ptr->parms_id(), destination_size);
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::mod_switch_scale_to_next(
        const Ciphertext &encrypted, Ciphertext &destination, MemoryPoolHandle pool)
    {
        // Assuming at this point encrypted is already validated.
        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        if (context_data_ptr->parms().scheme() == scheme_type::BFV && encrypted.is_ntt_form())
        {
            throw invalid_argument("BFV encrypted cannot be in NTT form");
        }
        if (context_data_ptr->parms().scheme() == scheme_type::CKKS && !encrypted.is_ntt_form())
        {
            throw invalid_argument("CKKS encrypted must be in NTT form");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }

        // Extract encryption parameters.
        auto &context_data = *context_data_ptr;
        auto &next_context_data = *context_data.next_context_data();
        auto &next_parms = next_context_data.parms();
        auto rns_tool = context_data.rns_tool();

        size_t encrypted_size = encrypted.size();
        size_t coeff_count = next_parms.poly_modulus_degree();
        size_t next_coeff_modulus_count = next_parms.coeff_modulus().size();

        Ciphertext encrypted_copy(pool);
        encrypted_copy = encrypted;

        switch (next_parms.scheme())
        {
        case scheme_type::BFV:
            for_each_n(PolyIterator(encrypted_copy), encrypted_size, [&](auto I) {
                rns_tool->divide_and_round_q_last_inplace(I, pool);
            });
            break;

        case scheme_type::CKKS:
            for_each_n(PolyIterator(encrypted_copy), encrypted_size, [&](auto I) {
                rns_tool->divide_and_round_q_last_ntt_inplace(I, context_data.small_ntt_tables(), pool);
            });
            break;

        default:
            throw invalid_argument("unsupported scheme");
        }

        // Copy result to destination
        destination.resize(context_, next_context_data.parms_id(), encrypted_size);
        for_each_n(
            IteratorTuple<ConstPolyIterator, PolyIterator>(encrypted_copy, destination), encrypted_size,
            [&](auto I) { set_poly_poly(get<0>(I), coeff_count, next_coeff_modulus_count, get<1>(I)); });

        // Set other attributes
        destination.is_ntt_form() = encrypted.is_ntt_form();
        if (next_parms.scheme() == scheme_type::CKKS)
        {
            // Change the scale when using CKKS
            destination.scale() =
                encrypted.scale() / static_cast<double>(context_data.parms().coeff_modulus().back().value());
        }
    }

    void Evaluator::mod_switch_drop_to_next(const Ciphertext &encrypted, Ciphertext &destination, MemoryPoolHandle pool)
    {
        // Assuming at this point encrypted is already validated.
        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        if (context_data_ptr->parms().scheme() == scheme_type::CKKS && !encrypted.is_ntt_form())
        {
            throw invalid_argument("CKKS encrypted must be in NTT form");
        }

        // Extract encryption parameters.
        auto &next_context_data = *context_data_ptr->next_context_data();
        auto &next_parms = next_context_data.parms();

        // Check that scale is positive and not too large
        if (encrypted.scale() <= 0 ||
            (static_cast<int>(log2(encrypted.scale())) >= next_context_data.total_coeff_modulus_bit_count()))
        {
            throw invalid_argument("scale out of bounds");
        }

        // q_1,...,q_{k-1}
        size_t next_coeff_modulus_count = next_parms.coeff_modulus().size();
        size_t coeff_count = next_parms.poly_modulus_degree();
        size_t encrypted_size = encrypted.size();

        // Size check
        if (!product_fits_in(encrypted_size, coeff_count, next_coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        size_t rns_poly_total_count = next_coeff_modulus_count * coeff_count;

        auto drop_moduli_and_copy = [&](ConstPolyIterator in_iter, PolyIterator out_iter) {
            for_each_n(IteratorTuple<ConstPolyIterator, PolyIterator>(in_iter, out_iter), encrypted_size, [&](auto I) {
                for_each_n(
                    I, next_coeff_modulus_count, [&](auto J) { set_uint_uint(get<0>(J), coeff_count, get<1>(J)); });
            });
        };

        if (&encrypted == &destination)
        {
            // Switching in-place so need temporary space
            auto temp(allocate_uint(rns_poly_total_count * encrypted_size, pool));

            // Copy data over to temp; only copy the RNS components relevant after modulus drop
            drop_moduli_and_copy(encrypted, { temp.get(), coeff_count, next_coeff_modulus_count });

            // Resize destination before writing
            destination.resize(context_, next_context_data.parms_id(), encrypted_size);
            destination.is_ntt_form() = true;
            destination.scale() = encrypted.scale();

            // Copy data to destination
            set_uint_uint(temp.get(), rns_poly_total_count * encrypted_size, destination.data());
        }
        else
        {
            // Resize destination before writing
            destination.resize(context_, next_context_data.parms_id(), encrypted_size);
            destination.is_ntt_form() = true;
            destination.scale() = encrypted.scale();

            // Copy data over to destination; only copy the RNS components relevant after modulus drop
            drop_moduli_and_copy(encrypted, destination);
        }
    }

    void Evaluator::mod_switch_drop_to_next(Plaintext &plain)
    {
        // Assuming at this point plain is already validated.
        auto context_data_ptr = context_->get_context_data(plain.parms_id());
        if (!plain.is_ntt_form())
        {
            throw invalid_argument("plain is not in NTT form");
        }
        if (!context_data_ptr->next_context_data())
        {
            throw invalid_argument("end of modulus switching chain reached");
        }

        // Extract encryption parameters.
        auto &next_context_data = *context_data_ptr->next_context_data();
        auto &next_parms = context_data_ptr->next_context_data()->parms();

        // Check that scale is positive and not too large
        if (plain.scale() <= 0 ||
            (static_cast<int>(log2(plain.scale())) >= next_context_data.total_coeff_modulus_bit_count()))
        {
            throw invalid_argument("scale out of bounds");
        }

        // q_1,...,q_{k-1}
        auto &next_coeff_modulus = next_parms.coeff_modulus();
        size_t next_coeff_modulus_count = next_coeff_modulus.size();
        size_t coeff_count = next_parms.poly_modulus_degree();

        // Compute destination size first for exception safety
        auto dest_size = mul_safe(next_coeff_modulus_count, coeff_count);

        plain.parms_id() = parms_id_zero;
        plain.resize(dest_size);
        plain.parms_id() = next_context_data.parms_id();
    }

    void Evaluator::mod_switch_to_next(const Ciphertext &encrypted, Ciphertext &destination, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }

        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        if (context_->last_parms_id() == encrypted.parms_id())
        {
            throw invalid_argument("end of modulus switching chain reached");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }

        switch (context_->first_context_data()->parms().scheme())
        {
        case scheme_type::BFV:
            // Modulus switching with scaling
            mod_switch_scale_to_next(encrypted, destination, move(pool));
            break;

        case scheme_type::CKKS:
            // Modulus switching without scaling
            mod_switch_drop_to_next(encrypted, destination, move(pool));
            break;

        default:
            throw invalid_argument("unsupported scheme");
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (destination.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::mod_switch_to_inplace(Ciphertext &encrypted, parms_id_type parms_id, MemoryPoolHandle pool)
    {
        // Verify parameters.
        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        auto target_context_data_ptr = context_->get_context_data(parms_id);
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!target_context_data_ptr)
        {
            throw invalid_argument("parms_id is not valid for encryption parameters");
        }
        if (context_data_ptr->chain_index() < target_context_data_ptr->chain_index())
        {
            throw invalid_argument("cannot switch to higher level modulus");
        }

        while (encrypted.parms_id() != parms_id)
        {
            mod_switch_to_next_inplace(encrypted, pool);
        }
    }

    void Evaluator::mod_switch_to_inplace(Plaintext &plain, parms_id_type parms_id)
    {
        // Verify parameters.
        auto context_data_ptr = context_->get_context_data(plain.parms_id());
        auto target_context_data_ptr = context_->get_context_data(parms_id);
        if (!context_data_ptr)
        {
            throw invalid_argument("plain is not valid for encryption parameters");
        }
        if (!context_->get_context_data(parms_id))
        {
            throw invalid_argument("parms_id is not valid for encryption parameters");
        }
        if (!plain.is_ntt_form())
        {
            throw invalid_argument("plain is not in NTT form");
        }
        if (context_data_ptr->chain_index() < target_context_data_ptr->chain_index())
        {
            throw invalid_argument("cannot switch to higher level modulus");
        }

        while (plain.parms_id() != parms_id)
        {
            mod_switch_to_next_inplace(plain);
        }
    }

    void Evaluator::rescale_to_next(const Ciphertext &encrypted, Ciphertext &destination, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (context_->last_parms_id() == encrypted.parms_id())
        {
            throw invalid_argument("end of modulus switching chain reached");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }

        switch (context_->first_context_data()->parms().scheme())
        {
        case scheme_type::BFV:
            throw invalid_argument("unsupported operation for scheme type");

        case scheme_type::CKKS:
            // Modulus switching with scaling
            mod_switch_scale_to_next(encrypted, destination, move(pool));
            break;

        default:
            throw invalid_argument("unsupported scheme");
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (destination.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::rescale_to_inplace(Ciphertext &encrypted, parms_id_type parms_id, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }

        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        auto target_context_data_ptr = context_->get_context_data(parms_id);
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!target_context_data_ptr)
        {
            throw invalid_argument("parms_id is not valid for encryption parameters");
        }
        if (context_data_ptr->chain_index() < target_context_data_ptr->chain_index())
        {
            throw invalid_argument("cannot switch to higher level modulus");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }

        switch (context_data_ptr->parms().scheme())
        {
        case scheme_type::BFV:
            throw invalid_argument("unsupported operation for scheme type");

        case scheme_type::CKKS:
            while (encrypted.parms_id() != parms_id)
            {
                // Modulus switching with scaling
                mod_switch_scale_to_next(encrypted, encrypted, pool);
            }
            break;

        default:
            throw invalid_argument("unsupported scheme");
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::multiply_many(
        const vector<Ciphertext> &encrypteds, const RelinKeys &relin_keys, Ciphertext &destination,
        MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (encrypteds.size() == 0)
        {
            throw invalid_argument("encrypteds vector must not be empty");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }
        for (size_t i = 0; i < encrypteds.size(); i++)
        {
            if (&encrypteds[i] == &destination)
            {
                throw invalid_argument("encrypteds must be different from destination");
            }
        }

        // There is at least one ciphertext
        auto context_data_ptr = context_->get_context_data(encrypteds[0].parms_id());
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypteds is not valid for encryption parameters");
        }

        // Extract encryption parameters.
        auto &context_data = *context_data_ptr;
        auto &parms = context_data.parms();

        if (parms.scheme() != scheme_type::BFV)
        {
            throw logic_error("unsupported scheme");
        }

        // If there is only one ciphertext, return it.
        if (encrypteds.size() == 1)
        {
            destination = encrypteds[0];
            return;
        }

        // Do first level of multiplications
        vector<Ciphertext> product_vec;
        for (size_t i = 0; i < encrypteds.size() - 1; i += 2)
        {
            Ciphertext temp(context_, context_data.parms_id(), pool);
            if (encrypteds[i].data() == encrypteds[i + 1].data())
            {
                square(encrypteds[i], temp);
            }
            else
            {
                multiply(encrypteds[i], encrypteds[i + 1], temp);
            }
            relinearize_inplace(temp, relin_keys, pool);
            product_vec.emplace_back(move(temp));
        }
        if (encrypteds.size() & 1)
        {
            product_vec.emplace_back(encrypteds.back());
        }

        // Repeatedly multiply and add to the back of the vector until the end is reached
        for (size_t i = 0; i < product_vec.size() - 1; i += 2)
        {
            Ciphertext temp(context_, context_data.parms_id(), pool);
            multiply(product_vec[i], product_vec[i + 1], temp);
            relinearize_inplace(temp, relin_keys, pool);
            product_vec.emplace_back(move(temp));
        }

        destination = product_vec.back();
    }

    void Evaluator::exponentiate_inplace(
        Ciphertext &encrypted, uint64_t exponent, const RelinKeys &relin_keys, MemoryPoolHandle pool)
    {
        // Verify parameters.
        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!context_->get_context_data(relin_keys.parms_id()))
        {
            throw invalid_argument("relin_keys is not valid for encryption parameters");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }
        if (exponent == 0)
        {
            throw invalid_argument("exponent cannot be 0");
        }

        // Fast case
        if (exponent == 1)
        {
            return;
        }

        // Create a vector of copies of encrypted
        vector<Ciphertext> exp_vector(static_cast<size_t>(exponent), encrypted);
        multiply_many(exp_vector, relin_keys, encrypted, move(pool));
    }

    void Evaluator::add_plain_inplace(Ciphertext &encrypted, const Plaintext &plain)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!is_metadata_valid_for(plain, context_) || !is_buffer_valid(plain))
        {
            throw invalid_argument("plain is not valid for encryption parameters");
        }

        auto &context_data = *context_->get_context_data(encrypted.parms_id());
        auto &parms = context_data.parms();
        if (parms.scheme() == scheme_type::BFV && encrypted.is_ntt_form())
        {
            throw invalid_argument("BFV encrypted cannot be in NTT form");
        }
        if (parms.scheme() == scheme_type::CKKS && !encrypted.is_ntt_form())
        {
            throw invalid_argument("CKKS encrypted must be in NTT form");
        }
        if (plain.is_ntt_form() != encrypted.is_ntt_form())
        {
            throw invalid_argument("NTT form mismatch");
        }
        if (encrypted.is_ntt_form() && (encrypted.parms_id() != plain.parms_id()))
        {
            throw invalid_argument("encrypted and plain parameter mismatch");
        }
        if (!are_same_scale(encrypted, plain))
        {
            throw invalid_argument("scale mismatch");
        }

        // Extract encryption parameters.
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();

        // Size check
        if (!product_fits_in(coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        switch (parms.scheme())
        {
        case scheme_type::BFV:
        {
            multiply_add_plain_with_scaling_variant(plain, context_data, encrypted.data());
            break;
        }

        case scheme_type::CKKS:
        {
            RNSIterator encrypted_iter(encrypted.data(), coeff_count);
            ConstRNSIterator plain_iter(plain.data(), coeff_count);
            for_each_n(
                IteratorTuple<RNSIterator, ConstRNSIterator, IteratorWrapper<const SmallModulus *>>(
                    encrypted_iter, plain_iter, coeff_modulus),
                coeff_modulus_count,
                [&](auto I) { add_poly_poly_coeffmod(get<0>(I), get<1>(I), coeff_count, *get<2>(I), get<0>(I)); });
            break;
        }

        default:
            throw invalid_argument("unsupported scheme");
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::sub_plain_inplace(Ciphertext &encrypted, const Plaintext &plain)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!is_metadata_valid_for(plain, context_) || !is_buffer_valid(plain))
        {
            throw invalid_argument("plain is not valid for encryption parameters");
        }

        auto &context_data = *context_->get_context_data(encrypted.parms_id());
        auto &parms = context_data.parms();
        if (parms.scheme() == scheme_type::BFV && encrypted.is_ntt_form())
        {
            throw invalid_argument("BFV encrypted cannot be in NTT form");
        }
        if (parms.scheme() == scheme_type::CKKS && !encrypted.is_ntt_form())
        {
            throw invalid_argument("CKKS encrypted must be in NTT form");
        }
        if (plain.is_ntt_form() != encrypted.is_ntt_form())
        {
            throw invalid_argument("NTT form mismatch");
        }
        if (encrypted.is_ntt_form() && (encrypted.parms_id() != plain.parms_id()))
        {
            throw invalid_argument("encrypted and plain parameter mismatch");
        }
        if (!are_same_scale(encrypted, plain))
        {
            throw invalid_argument("scale mismatch");
        }

        // Extract encryption parameters.
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();

        // Size check
        if (!product_fits_in(coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        switch (parms.scheme())
        {
        case scheme_type::BFV:
        {
            multiply_sub_plain_with_scaling_variant(plain, context_data, encrypted.data());
            break;
        }

        case scheme_type::CKKS:
        {
            RNSIterator encrypted_iter(encrypted.data(), coeff_count);
            ConstRNSIterator plain_iter(plain.data(), coeff_count);
            for_each_n(
                IteratorTuple<RNSIterator, ConstRNSIterator, IteratorWrapper<const SmallModulus *>>(
                    encrypted_iter, plain_iter, coeff_modulus),
                coeff_modulus_count,
                [&](auto I) { sub_poly_poly_coeffmod(get<0>(I), get<1>(I), coeff_count, *get<2>(I), get<0>(I)); });
            break;
        }

        default:
            throw invalid_argument("unsupported scheme");
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::multiply_plain_inplace(Ciphertext &encrypted, const Plaintext &plain, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!is_metadata_valid_for(plain, context_) || !is_buffer_valid(plain))
        {
            throw invalid_argument("plain is not valid for encryption parameters");
        }
        if (encrypted.is_ntt_form() != plain.is_ntt_form())
        {
            throw invalid_argument("NTT form mismatch");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }

        if (encrypted.is_ntt_form())
        {
            multiply_plain_ntt(encrypted, plain);
        }
        else
        {
            multiply_plain_normal(encrypted, plain, move(pool));
        }
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::multiply_plain_normal(Ciphertext &encrypted, const Plaintext &plain, MemoryPoolHandle pool)
    {
        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted.parms_id());
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();

        auto plain_upper_half_threshold = context_data.plain_upper_half_threshold();
        auto plain_upper_half_increment = context_data.plain_upper_half_increment();
        auto coeff_modulus_ntt_tables = context_data.small_ntt_tables();

        size_t encrypted_size = encrypted.size();
        size_t plain_coeff_count = plain.coeff_count();
        size_t plain_nonzero_coeff_count = plain.nonzero_coeff_count();

        // Size check
        if (!product_fits_in(encrypted_size, coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        double new_scale = encrypted.scale() * plain.scale();

        // Check that scale is positive and not too large
        if (new_scale <= 0 || (static_cast<int>(log2(new_scale)) >= context_data.total_coeff_modulus_bit_count()))
        {
            throw invalid_argument("scale out of bounds");
        }

        // Set the scale
        encrypted.scale() = new_scale;

        /*
        Optimizations for constant / monomial multiplication can lead to the presence of a timing side-channel in
        use-cases where the plaintext data should also be kept private.
        */
        if (plain_nonzero_coeff_count == 1)
        {
            // Multiplying by a monomial?
            size_t mono_exponent = plain.significant_coeff_count() - 1;

            // RNS monomial multiplication: monomial and multiplicand polynomial are in RNS form
            auto rns_monomial_multiply = [&](PolyIterator in_iter, ConstCoeffIterator mono_iter) {
                for_each_n(in_iter, encrypted_size, [&](auto I) {
                    for_each_n(
                        IteratorTuple<RNSIterator, ConstCoeffIterator, IteratorWrapper<const SmallModulus *>>(
                            I, mono_iter, coeff_modulus),
                        coeff_modulus_count, [&](auto J) {
                            negacyclic_multiply_poly_mono_coeffmod(
                                get<0>(J), coeff_count, *get<1>(J), mono_exponent, *get<2>(J), get<0>(J), pool);
                        });
                });
            };

            if (plain[mono_exponent] >= plain_upper_half_threshold)
            {
                if (!context_data.qualifiers().using_fast_plain_lift)
                {
                    // Allocate temporary space for a single RNS coefficient
                    auto temp(allocate_uint(coeff_modulus_count, pool));

                    // We need to adjust the monomial modulo each coeff_modulus prime separately when the coeff_modulus
                    // primes may be larger than the plain_modulus. We add plain_upper_half_increment (i.e., q-t) to
                    // the monomial to ensure it is smaller than coeff_modulus and then do an RNS multiplication. Note
                    // that in this case plain_upper_half_increment contains a multi-precision integer, so after the
                    // addition we decompose the multi-precision integer into RNS components, and then multiply.
                    add_uint_uint64(plain_upper_half_increment, plain[mono_exponent], coeff_modulus_count, temp.get());
                    context_data.rns_tool()->base_q()->decompose(temp.get(), pool);
                    rns_monomial_multiply(encrypted, temp.get());
                }
                else
                {
                    // Every coeff_modulus prime is larger than plain_modulus, so there is no need to adjust the
                    // monomial. Instead, just do an RNS multiplication.
                    rns_monomial_multiply(encrypted, plain.data());
                }
            }
            else
            {
                // The monomial represents a positive number, so no RNS multiplication is needed.
                for_each_n(PolyIterator(encrypted), encrypted_size, [&](auto I) {
                    for_each_n(
                        IteratorTuple<RNSIterator, IteratorWrapper<const SmallModulus *>>(I, coeff_modulus),
                        coeff_modulus_count, [&](auto J) {
                            negacyclic_multiply_poly_mono_coeffmod(
                                get<0>(J), coeff_count, plain[mono_exponent], mono_exponent, *get<1>(J), get<0>(J),
                                pool);
                        });
                });
            }

            return;
        }

        // Generic case: any plaintext polynomial
        // Allocate temporary space for an entire RNS polynomial
        auto temp(allocate_zero_uint(coeff_count * coeff_modulus_count, pool));

        if (!context_data.qualifiers().using_fast_plain_lift)
        {
            // Slight semantic misuse of RNSIterator here, but this works well
            RNSIterator temp_iter(temp.get(), coeff_modulus_count);

            for_each_n(
                IteratorTuple<ConstCoeffIterator, RNSIterator>(plain.data(), temp_iter), plain_coeff_count,
                [&](auto I) {
                    auto plain_value = *get<0>(I);
                    if (plain_value >= plain_upper_half_threshold)
                    {
                        add_uint_uint64(plain_upper_half_increment, plain_value, coeff_modulus_count, get<1>(I));
                    }
                    else
                    {
                        **get<1>(I) = plain_value;
                    }
                });

            for_each_n(RNSIterator(temp.get(), coeff_count), coeff_modulus_count, [&](auto I) {
                context_data.rns_tool()->base_q()->decompose_array(I, coeff_count, pool);
            });
        }
        else
        {
            // Note that in this case plain_upper_half_increment holds its value in RNS form modulo the coeff_modulus
            // primes.
            RNSIterator temp_iter(temp.get(), coeff_count);
            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const uint64_t *>>(temp_iter, plain_upper_half_increment),
                coeff_modulus_count, [&](auto I) {
                    SEAL_ASSERT_TYPE(get<0>(I), CoeffIterator, "temp");
                    SEAL_ASSERT_TYPE(get<1>(I), const uint64_t *, "plain_upper_half_increment");
                    for_each_n(
                        IteratorTuple<CoeffIterator, ConstCoeffIterator>(get<0>(I), plain.data()), plain_coeff_count,
                        [&](auto J) {
                            SEAL_ASSERT_TYPE(get<0>(J), uint64_t *, "temp");
                            SEAL_ASSERT_TYPE(get<1>(J), const uint64_t *, "plain");
                            *get<0>(J) = *get<1>(J) + (*get<1>(I) & static_cast<uint64_t>(-static_cast<int64_t>(
                                                                        *get<1>(J) >= plain_upper_half_threshold)));
                        });
                });
        }

        // Need to multiply each component in encrypted with temp; first step is to transform to NTT form
        RNSIterator temp_iter(temp.get(), coeff_count);
        for_each_n(
            IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(temp_iter, coeff_modulus_ntt_tables),
            coeff_modulus_count, [&](auto I) { ntt_negacyclic_harvey(get<0>(I), *get<1>(I)); });

        for_each_n(PolyIterator(encrypted), encrypted_size, [&](auto I) {
            for_each_n(
                IteratorTuple<
                    RNSIterator, ConstRNSIterator, IteratorWrapper<const SmallModulus *>,
                    IteratorWrapper<const SmallNTTTables *>>(I, temp_iter, coeff_modulus, coeff_modulus_ntt_tables),
                coeff_modulus_count, [&](auto J) {
                    SEAL_ASSERT_TYPE(get<0>(J), CoeffIterator, "encrypted");
                    SEAL_ASSERT_TYPE(get<1>(J), ConstCoeffIterator, "temp");
                    SEAL_ASSERT_TYPE(get<2>(J), const SmallModulus *, "coeff_modulus");
                    SEAL_ASSERT_TYPE(get<3>(J), const SmallNTTTables *, "coeff_modulus_ntt_tables");

                    // Lazy reduction
                    ntt_negacyclic_harvey_lazy(get<0>(J), *get<3>(J));

                    dyadic_product_coeffmod(get<0>(J), get<1>(J), coeff_count, *get<2>(J), get<0>(J));
                    inverse_ntt_negacyclic_harvey(get<0>(J), *get<3>(J));
                });
        });
    }

    void Evaluator::multiply_plain_ntt(Ciphertext &encrypted_ntt, const Plaintext &plain_ntt)
    {
        // Verify parameters.
        if (!plain_ntt.is_ntt_form())
        {
            throw invalid_argument("plain_ntt is not in NTT form");
        }
        if (encrypted_ntt.parms_id() != plain_ntt.parms_id())
        {
            throw invalid_argument("encrypted_ntt and plain_ntt parameter mismatch");
        }

        // Extract encryption parameters.
        auto &context_data = *context_->get_context_data(encrypted_ntt.parms_id());
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();
        size_t encrypted_ntt_size = encrypted_ntt.size();

        // Size check
        if (!product_fits_in(encrypted_ntt_size, coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        double new_scale = encrypted_ntt.scale() * plain_ntt.scale();

        // Check that scale is positive and not too large
        if (new_scale <= 0 || (static_cast<int>(log2(new_scale)) >= context_data.total_coeff_modulus_bit_count()))
        {
            throw invalid_argument("scale out of bounds");
        }

        ConstRNSIterator plain_ntt_iter(plain_ntt.data(), coeff_count);
        for_each_n(PolyIterator(encrypted_ntt), encrypted_ntt_size, [&](auto I) {
            for_each_n(
                IteratorTuple<RNSIterator, ConstRNSIterator, IteratorWrapper<const SmallModulus *>>(
                    I, plain_ntt_iter, coeff_modulus),
                coeff_modulus_count,
                [&](auto J) { dyadic_product_coeffmod(get<0>(J), get<1>(J), coeff_count, *get<2>(J), get<0>(J)); });
        });

        // Set the scale
        encrypted_ntt.scale() = new_scale;
    }

    void Evaluator::transform_to_ntt_inplace(Plaintext &plain, parms_id_type parms_id, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_valid_for(plain, context_))
        {
            throw invalid_argument("plain is not valid for encryption parameters");
        }

        auto context_data_ptr = context_->get_context_data(parms_id);
        if (!context_data_ptr)
        {
            throw invalid_argument("parms_id is not valid for the current context");
        }
        if (plain.is_ntt_form())
        {
            throw invalid_argument("plain is already in NTT form");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }

        // Extract encryption parameters.
        auto &context_data = *context_data_ptr;
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();
        size_t plain_coeff_count = plain.coeff_count();

        auto plain_upper_half_threshold = context_data.plain_upper_half_threshold();
        auto plain_upper_half_increment = context_data.plain_upper_half_increment();

        auto coeff_modulus_ntt_tables = context_data.small_ntt_tables();

        // Size check
        if (!product_fits_in(coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        // Resize to fit the entire NTT transformed (ciphertext size) polynomial
        // Note that the new coefficients are automatically set to 0
        plain.resize(coeff_count * coeff_modulus_count);

        if (!context_data.qualifiers().using_fast_plain_lift)
        {
            // Allocate temporary space for an entire RNS polynomial
            auto temp(allocate_zero_uint(coeff_count * coeff_modulus_count, pool));

            // Slight semantic misuse of RNSIterator here, but this works well
            RNSIterator temp_iter(temp.get(), coeff_modulus_count);

            for_each_n(
                IteratorTuple<ConstCoeffIterator, RNSIterator>(plain.data(), temp_iter), plain_coeff_count,
                [&](auto I) {
                    auto plain_value = *get<0>(I);
                    if (plain_value >= plain_upper_half_threshold)
                    {
                        add_uint_uint64(plain_upper_half_increment, plain_value, coeff_modulus_count, get<1>(I));
                    }
                    else
                    {
                        **get<1>(I) = plain_value;
                    }
                });

            for_each_n(RNSIterator(temp.get(), coeff_count), coeff_modulus_count, [&](auto I) {
                context_data.rns_tool()->base_q()->decompose_array(I, coeff_count, pool);
            });

            // Copy data back to plain
            set_poly_poly(temp.get(), coeff_count, coeff_modulus_count, plain.data());
        }
        else
        {
            // Note that in this case plain_upper_half_increment holds its value in RNS form modulo the coeff_modulus
            // primes.
            RNSIterator plain_iter(plain.data(), coeff_count);

            // Create a "reversed" helper iterator that iterates in the reverse order both plain RNS components and
            // the plain_upper_half_increment values.
            IteratorTuple<RNSIterator, IteratorWrapper<const uint64_t *>> helper_iter(
                plain_iter, plain_upper_half_increment);
            advance(helper_iter, coeff_modulus_count - 1);
            auto reversed_helper_iter = ReverseIterator<decltype(helper_iter)>(helper_iter);

            for_each_n(reversed_helper_iter, coeff_modulus_count, [&](auto I) {
                SEAL_ASSERT_TYPE(get<0>(I), CoeffIterator, "reversed plain");
                SEAL_ASSERT_TYPE(get<1>(I), const uint64_t *, "reversed plain_upper_half_increment");

                for_each_n(
                    IteratorTuple<CoeffIterator, CoeffIterator>(*plain_iter, get<0>(I)), plain_coeff_count,
                    [&](auto J) {
                        SEAL_ASSERT_TYPE(get<0>(J), uint64_t *, "plain");
                        SEAL_ASSERT_TYPE(get<1>(J), uint64_t *, "reversed plain");
                        *get<1>(J) = *get<0>(J) + (*get<1>(I) & static_cast<uint64_t>(-static_cast<int64_t>(
                                                                    *get<0>(J) >= plain_upper_half_threshold)));
                    });
            });
        }

        // Transform to NTT domain
        RNSIterator plain_iter(plain.data(), coeff_count);
        for_each_n(
            IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(plain_iter, coeff_modulus_ntt_tables),
            coeff_modulus_count, [&](auto I) { ntt_negacyclic_harvey(get<0>(I), *get<1>(I)); });

        plain.parms_id() = parms_id;
    }

    void Evaluator::transform_to_ntt_inplace(Ciphertext &encrypted)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }

        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (encrypted.is_ntt_form())
        {
            throw invalid_argument("encrypted is already in NTT form");
        }

        // Extract encryption parameters.
        auto &context_data = *context_data_ptr;
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();
        size_t encrypted_size = encrypted.size();

        auto coeff_modulus_ntt_tables = context_data.small_ntt_tables();

        // Size check
        if (!product_fits_in(coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        // Transform each polynomial to NTT domain
        for_each_n(PolyIterator(encrypted), encrypted_size, [&](auto I) {
            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(I, coeff_modulus_ntt_tables),
                coeff_modulus_count, [&](auto J) { ntt_negacyclic_harvey(get<0>(J), *get<1>(J)); });
        });

        // Finally change the is_ntt_transformed flag
        encrypted.is_ntt_form() = true;
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::transform_from_ntt_inplace(Ciphertext &encrypted_ntt)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted_ntt, context_) || !is_buffer_valid(encrypted_ntt))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }

        auto context_data_ptr = context_->get_context_data(encrypted_ntt.parms_id());
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypted_ntt is not valid for encryption parameters");
        }
        if (!encrypted_ntt.is_ntt_form())
        {
            throw invalid_argument("encrypted_ntt is not in NTT form");
        }

        // Extract encryption parameters.
        auto &context_data = *context_data_ptr;
        auto &parms = context_data.parms();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = parms.coeff_modulus().size();
        size_t encrypted_ntt_size = encrypted_ntt.size();

        auto coeff_modulus_ntt_tables = context_data.small_ntt_tables();

        // Size check
        if (!product_fits_in(coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        // Transform each polynomial from NTT domain
        for_each_n(PolyIterator(encrypted_ntt), encrypted_ntt_size, [&](auto I) {
            for_each_n(
                IteratorTuple<RNSIterator, IteratorWrapper<const SmallNTTTables *>>(I, coeff_modulus_ntt_tables),
                coeff_modulus_count, [&](auto J) { inverse_ntt_negacyclic_harvey(get<0>(J), *get<1>(J)); });
        });

        // Finally change the is_ntt_transformed flag
        encrypted_ntt.is_ntt_form() = false;
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted_ntt.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::apply_galois_inplace(
        Ciphertext &encrypted, uint32_t galois_elt, const GaloisKeys &galois_keys, MemoryPoolHandle pool)
    {
        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }

        // Don't validate all of galois_keys but just check the parms_id.
        if (galois_keys.parms_id() != context_->key_parms_id())
        {
            throw invalid_argument("galois_keys is not valid for encryption parameters");
        }

        auto &context_data = *context_->get_context_data(encrypted.parms_id());
        auto &parms = context_data.parms();
        auto &coeff_modulus = parms.coeff_modulus();
        size_t coeff_count = parms.poly_modulus_degree();
        size_t coeff_modulus_count = coeff_modulus.size();
        size_t encrypted_size = encrypted.size();
        // Use key_context_data where permutation tables exist since previous runs.
        auto galois_tool = context_->key_context_data()->galois_tool();

        // Size check
        if (!product_fits_in(coeff_count, coeff_modulus_count))
        {
            throw logic_error("invalid parameters");
        }

        // Check if Galois key is generated or not.
        if (!galois_keys.has_key(galois_elt))
        {
            throw invalid_argument("Galois key not present");
        }

        uint64_t m = mul_safe(static_cast<uint64_t>(coeff_count), uint64_t(2));

        // Verify parameters
        if (!(galois_elt & 1) || unsigned_geq(galois_elt, m))
        {
            throw invalid_argument("Galois element is not valid");
        }
        if (encrypted_size > 2)
        {
            throw invalid_argument("encrypted size must be 2");
        }

        auto temp(allocate_poly(coeff_count, coeff_modulus_count, pool));
        RNSIterator temp_iter(temp.get(), coeff_count);

        // DO NOT CHANGE EXECUTION ORDER OF FOLLOWING SECTION
        // BEGIN: Apply Galois for each ciphertext
        // Execution order is sensitive, since apply_galois is not inplace!
        if (parms.scheme() == scheme_type::BFV)
        {
            auto apply_galois_helper = [&](auto in_iter, auto out_iter) {
                for_each_n(
                    IteratorTuple<RNSIterator, RNSIterator, IteratorWrapper<const SmallModulus *>>(
                        in_iter, out_iter, coeff_modulus),
                    coeff_modulus_count,
                    [&](auto I) { galois_tool->apply_galois(get<0>(I), galois_elt, *get<2>(I), get<1>(I)); });
            };

            // !!! DO NOT CHANGE EXECUTION ORDER!!!

            // First transform encrypted.data(0)
            PolyIterator encrypted_iter(encrypted);
            apply_galois_helper(*encrypted_iter++, temp_iter);

            // Copy result to encrypted.data(0)
            set_poly_poly(temp.get(), coeff_count, coeff_modulus_count, encrypted.data(0));

            // Next transform encrypted.data(1)
            apply_galois_helper(*encrypted_iter, temp_iter);
        }
        else if (parms.scheme() == scheme_type::CKKS)
        {
            auto apply_galois_helper_ntt = [&](auto in_iter, auto out_iter) {
                for_each_n(
                    IteratorTuple<RNSIterator, RNSIterator>(in_iter, out_iter), coeff_modulus_count, [&](auto I) {
                        const_cast<GaloisTool *>(galois_tool)->apply_galois_ntt(get<0>(I), galois_elt, get<1>(I));
                    });
            };

            // !!! DO NOT CHANGE EXECUTION ORDER!!!

            // First transform encrypted.data(0)
            PolyIterator encrypted_iter(encrypted);
            apply_galois_helper_ntt(*encrypted_iter++, temp_iter);

            // Copy result to encrypted.data(0)
            set_poly_poly(temp.get(), coeff_count, coeff_modulus_count, encrypted.data(0));

            // Next transform encrypted.data(1)
            apply_galois_helper_ntt(*encrypted_iter, temp_iter);
        }
        else
        {
            throw logic_error("scheme not implemented");
        }

        // Wipe encrypted.data(1)
        set_zero_poly(coeff_count, coeff_modulus_count, encrypted.data(1));

        // END: Apply Galois for each ciphertext
        // REORDERING IS SAFE NOW

        // Calculate (temp * galois_key[0], temp * galois_key[1]) + (ct[0], 0)
        switch_key_inplace(
            encrypted, temp_iter, static_cast<const KSwitchKeys &>(galois_keys), GaloisKeys::get_index(galois_elt),
            pool);
#ifdef SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT
        // Transparent ciphertext output is not allowed.
        if (encrypted.is_transparent())
        {
            throw logic_error("result ciphertext is transparent");
        }
#endif
    }

    void Evaluator::rotate_internal(
        Ciphertext &encrypted, int steps, const GaloisKeys &galois_keys, MemoryPoolHandle pool)
    {
        auto context_data_ptr = context_->get_context_data(encrypted.parms_id());
        if (!context_data_ptr)
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!context_data_ptr->qualifiers().using_batching)
        {
            throw logic_error("encryption parameters do not support batching");
        }
        if (galois_keys.parms_id() != context_->key_parms_id())
        {
            throw invalid_argument("galois_keys is not valid for encryption parameters");
        }

        // Is there anything to do?
        if (steps == 0)
        {
            return;
        }

        size_t coeff_count = context_data_ptr->parms().poly_modulus_degree();
        auto galois_tool = context_data_ptr->galois_tool();

        // Check if Galois key is generated or not.
        if (galois_keys.has_key(galois_tool->get_elt_from_step(steps)))
        {
            // Perform rotation and key switching
            apply_galois_inplace(encrypted, galois_tool->get_elt_from_step(steps), galois_keys, move(pool));
        }
        else
        {
            // Convert the steps to NAF: guarantees using smallest HW
            vector<int> naf_steps = naf(steps);

            // If naf_steps contains only one element, then this is a power-of-two
            // rotation and we would have expected not to get to this part of the
            // if-statement.
            if (naf_steps.size() == 1)
            {
                throw invalid_argument("Galois key not present");
            }

            for_each_n(naf_steps.cbegin(), naf_steps.size(), [&](auto step)
            {
                // We might have a NAF-term of size coeff_count / 2; this corresponds
                // to no rotation so we skip it. Otherwise call rotate_internal.
                if (safe_cast<size_t>(abs(step)) != (coeff_count >> 1))
                {
                    // Apply rotation for this step
                    rotate_internal(encrypted, step, galois_keys, pool);
                }
            });
        }
    }

    void Evaluator::switch_key_inplace(
        Ciphertext &encrypted, ConstRNSIterator target, const KSwitchKeys &kswitch_keys, size_t kswitch_keys_index,
        MemoryPoolHandle pool)
    {
        auto parms_id = encrypted.parms_id();
        auto &context_data = *context_->get_context_data(parms_id);
        auto &parms = context_data.parms();
        auto &key_context_data = *context_->key_context_data();
        auto &key_parms = key_context_data.parms();
        auto scheme = parms.scheme();

        // Verify parameters.
        if (!is_metadata_valid_for(encrypted, context_) || !is_buffer_valid(encrypted))
        {
            throw invalid_argument("encrypted is not valid for encryption parameters");
        }
        if (!target)
        {
            throw invalid_argument("target");
        }
        if (!context_->using_keyswitching())
        {
            throw logic_error("keyswitching is not supported by the context");
        }

        // Don't validate all of kswitch_keys but just check the parms_id.
        if (kswitch_keys.parms_id() != context_->key_parms_id())
        {
            throw invalid_argument("parameter mismatch");
        }

        if (kswitch_keys_index >= kswitch_keys.data().size())
        {
            throw out_of_range("kswitch_keys_index");
        }
        if (!pool)
        {
            throw invalid_argument("pool is uninitialized");
        }
        if (scheme == scheme_type::BFV && encrypted.is_ntt_form())
        {
            throw invalid_argument("BFV encrypted cannot be in NTT form");
        }
        if (scheme == scheme_type::CKKS && !encrypted.is_ntt_form())
        {
            throw invalid_argument("CKKS encrypted must be in NTT form");
        }

        // Extract encryption parameters.
        size_t coeff_count = parms.poly_modulus_degree();
        size_t decomp_mod_count = parms.coeff_modulus().size();
        auto &key_modulus = key_parms.coeff_modulus();
        size_t key_mod_count = key_modulus.size();
        size_t rns_mod_count = decomp_mod_count + 1;
        auto small_ntt_tables = key_context_data.small_ntt_tables();
        auto modswitch_factors = key_context_data.rns_tool()->inv_q_last_mod_q();

        // Size check
        if (!product_fits_in(coeff_count, rns_mod_count, size_t(2)))
        {
            throw logic_error("invalid parameters");
        }

        // Prepare input
        auto &key_vector = kswitch_keys.data()[kswitch_keys_index];
        size_t key_component_count = key_vector[0].data().size();

        // Check only the used component in KSwitchKeys.
        for (auto &each_key : key_vector)
        {
            if (!is_metadata_valid_for(each_key, context_) || !is_buffer_valid(each_key))
            {
                throw invalid_argument("kswitch_keys is not valid for encryption parameters");
            }
        }

        // Temporary results
        Pointer<uint64_t> t_target(allocate_poly(coeff_count, decomp_mod_count, pool));
        RNSIterator t_target_iter(t_target.get(), coeff_count);
        set_uint_uint(target, decomp_mod_count * coeff_count, t_target.get());
        if (scheme == scheme_type::CKKS)
        {
            uint64_t *ptr = t_target.get();
            for (size_t i = 0; i < decomp_mod_count; i++, ptr += coeff_count)
            {
                inverse_ntt_negacyclic_harvey(ptr, small_ntt_tables[i]);
            }
        }
        // Ciphertext-side operand of switch key operation is in integer space now.

        // Temporary results
        auto t_poly_prod(allocate_zero_poly(coeff_count, rns_mod_count * key_component_count, pool));
        auto t_poly_lazy(allocate<unsigned long long>(mul_safe(coeff_count * 2, key_component_count), pool));
        auto t_ntt(allocate_uint(coeff_count, pool));

        for (size_t j = 0; j < rns_mod_count; j++)
        {
            size_t key_index = (j == decomp_mod_count ? key_mod_count - 1 : j);
            // Product of two numbers is up to 60 + 60 = 120 bits, so we can sum up to 256 of them without reduction.
            // Remark: This differs from the bound in uintarithsmallmod.cpp-->dot_product_mod.
#if SEAL_USER_MOD_BIT_COUNT_MAX > 32
            size_t lazy_reduction_summand_bound = 1 << (128 - SEAL_USER_MOD_BIT_COUNT_MAX * 2);
#else
            lazy_reduction_summand_bound = numeric_limits<size_t>::max();
#endif
            size_t lazy_reduction_counter = lazy_reduction_summand_bound;
            unsigned long long wide_product[2]{ 0, 0 };
            unsigned long long *accumulator = nullptr;
            uint64_t *t_target_acc = t_target.get();
            fill_n(t_poly_lazy.get(), mul_safe(coeff_count * 2, key_component_count), 0);

            // Multiply with keys and perform lazy reduction on product's coefficients
            for (size_t i = 0; i < decomp_mod_count; i++, t_target_acc += coeff_count)
            {
                const uint64_t *t_operand_ptr = nullptr;
                // RNS-NTT form exists in input
                if (scheme == scheme_type::CKKS && i == j)
                {
                    t_operand_ptr = static_cast<const uint64_t*>(target) + i * coeff_count;
                }
                // Perform RNS-NTT conversion
                else
                {
                    // No need to perform RNS conversion (modular reduction)
                    if (key_modulus[i].value() <= key_modulus[key_index].value())
                    {
                        set_uint_uint(t_target_acc, coeff_count, t_ntt.get());
                    }
                    // Perform RNS conversion (modular reduction)
                    else
                    {
                        modulo_poly_coeffs_63(t_target_acc, coeff_count, key_modulus[key_index], t_ntt.get());
                    }
                    // NTT conversion lazy outputs in [0, 4q)
                    ntt_negacyclic_harvey_lazy(t_ntt.get(), small_ntt_tables[key_index]);
                    t_operand_ptr = t_ntt.get();
                }
                // Multiply with keys and modular accumulate products in a lazy fashion
                accumulator = t_poly_lazy.get();
                for (size_t k = 0; k < key_component_count; k++)
                {
                    const uint64_t *t_key_acc = key_vector[i].data().data(k) + key_index * coeff_count;
                    if (!lazy_reduction_counter)
                    {
                        for (size_t l = 0; l < coeff_count; l++, t_key_acc++, accumulator += 2)
                        {
                            multiply_uint64(t_operand_ptr[l], *t_key_acc, wide_product);
                            // accumulate to t_poly_lazy
                            add_uint128(wide_product, accumulator, accumulator);
                            accumulator[0] = barrett_reduce_128(accumulator, key_modulus[key_index]);
                            accumulator[1] = 0;
                        }
                    }
                    else
                    {
                        for (size_t l = 0; l < coeff_count; l++, t_key_acc++, accumulator += 2)
                        {
                            multiply_uint64(t_operand_ptr[l], *t_key_acc, wide_product);
                            // accumulate to t_poly_lazy
                            add_uint128(wide_product, accumulator, accumulator);
                        }
                    }
                }
                if (!--lazy_reduction_counter)
                {
                    lazy_reduction_counter = lazy_reduction_summand_bound;
                }
            }

            // Final modular reduction
            accumulator = t_poly_lazy.get();
            for (size_t k = 0; k < key_component_count; k++)
            {
                uint64_t *t_poly_prod_acc = t_poly_prod.get() + (k * rns_mod_count + j) * coeff_count;
                if (lazy_reduction_counter == lazy_reduction_summand_bound)
                {
                    for (size_t l = 0; l < coeff_count; l++, accumulator += 2, t_poly_prod_acc++)
                    {
                        *t_poly_prod_acc = static_cast<uint64_t>(*accumulator);
                    }
                }
                else
                {
                    for (size_t l = 0; l < coeff_count; l++, accumulator += 2, t_poly_prod_acc++)
                    {
                        *t_poly_prod_acc = barrett_reduce_128(accumulator, key_modulus[key_index]);
                    }
                }
            }
        }
        // Accumulated products are now stored in t_poly_prod

        // Perform modulus switching with scaling
        for (size_t k = 0; k < key_component_count; k++)
        {
            uint64_t *encrypted_ptr = encrypted.data(k);
            uint64_t *t_poly_prod_ptr = t_poly_prod.get() + k * rns_mod_count * coeff_count;

            // Lazy reduction, they are then reduced mod qi
            uint64_t *t_last = t_poly_prod_ptr + decomp_mod_count * coeff_count;
            inverse_ntt_negacyclic_harvey_lazy(t_last, small_ntt_tables[key_mod_count - 1]);

            // Add (p-1)/2 to change from flooring to rounding.
            uint64_t half = key_modulus[key_mod_count - 1].value() >> 1;
            for (size_t l = 0; l < coeff_count; l++)
            {
                t_last[l] = barrett_reduce_63(t_last[l] + half, key_modulus[key_mod_count - 1]);
            }

            for (size_t j = 0; j < decomp_mod_count; j++)
            {
                uint64_t *t_else = t_poly_prod_ptr + j * coeff_count;
                // (ct mod 4qk) mod qi
                modulo_poly_coeffs_63(t_last, coeff_count, key_modulus[j], t_ntt.get());

                uint64_t fix = barrett_reduce_63(half, key_modulus[j]);
                for (size_t l = 0; l < coeff_count; l++)
                {
                    t_ntt.get()[l] = sub_uint_uint_mod(t_ntt.get()[l], fix, key_modulus[j]);
                }

                if (scheme == scheme_type::CKKS)
                {
                    ntt_negacyclic_harvey(t_ntt.get(), small_ntt_tables[j]);
                }
                else if (scheme == scheme_type::BFV)
                {
                    inverse_ntt_negacyclic_harvey(t_else, small_ntt_tables[j]);
                }
                // ((ct mod qi) - (ct mod qk)) mod qi
                sub_poly_poly_coeffmod(t_else, t_ntt.get(), coeff_count, key_modulus[j], t_else);
                // qk^(-1) * ((ct mod qi) - (ct mod qk)) mod qi
                multiply_poly_scalar_coeffmod(t_else, coeff_count, modswitch_factors[j], key_modulus[j], t_else);
                add_poly_poly_coeffmod(
                    t_else, encrypted_ptr + j * coeff_count, coeff_count, key_modulus[j],
                    encrypted_ptr + j * coeff_count);
            }
        }
    }
} // namespace seal
