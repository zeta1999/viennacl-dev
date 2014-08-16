#ifndef VIENNACL_DEVICE_SPECIFIC_TEMPLATES_TEMPLATE_BASE_
#define VIENNACL_DEVICE_SPECIFIC_TEMPLATES_TEMPLATE_BASE_

/* =========================================================================
   Copyright (c) 2010-2013, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
   Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                  ViennaCL - The Vienna Computing Library
                            -----------------

   Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

   (A list of authors and contributors can be found in the PDF manual)

   License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */


/** @file viennacl/generator/profile_base.hpp
 *
 * Base classes for the profiles
*/

#include <list>
#include <set>

#include "viennacl/ocl/kernel.hpp"
#include "viennacl/ocl/device.hpp"
#include "viennacl/ocl/device_utils.hpp"
#include "viennacl/ocl/infos.hpp"

#include "viennacl/scheduler/forwards.h"
#include "viennacl/scheduler/io.hpp"

#include "viennacl/device_specific/lazy_program_compiler.hpp"
#include "viennacl/device_specific/mapped_objects.hpp"
#include "viennacl/device_specific/tree_parsing.hpp"
#include "viennacl/device_specific/utils.hpp"

namespace viennacl
{

  namespace device_specific
  {

    enum fetching_policy_type
    {
      FETCH_FROM_LOCAL,
      FETCH_FROM_GLOBAL_STRIDED,
      FETCH_FROM_GLOBAL_CONTIGUOUS
    };

    class template_base
    {

      /** @brief Functor to map the statements to the types defined in mapped_objects.hpp */
      class map_functor : public tree_parsing::traversal_functor
      {

          scheduler::statement_node_numeric_type numeric_type(scheduler::statement const * statement, vcl_size_t root_idx) const
          {
              scheduler::statement_node const * root_node = &statement->array()[root_idx];
              while(root_node->lhs.numeric_type==scheduler::INVALID_NUMERIC_TYPE)
                  root_node = &statement->array()[root_node->lhs.node_index];
              return root_node->lhs.numeric_type;
          }

        public:
          typedef tools::shared_ptr<mapped_object> result_type;

          map_functor(symbolic_binder & binder, mapping_type & mapping) : binder_(binder), mapping_(mapping){ }

          /** @brief Binary leaf */
          template<class T>
          result_type binary_leaf(scheduler::statement const * statement, vcl_size_t root_idx, mapping_type const * mapping) const
          {
            return result_type(new T(utils::numeric_type_to_string(numeric_type(statement,root_idx)), binder_.get(NULL), mapped_object::node_info(mapping, statement, root_idx)));
          }

          template<class ScalarType>
          result_type operator()(ScalarType const & /*scalar*/) const
          {
            return result_type(new mapped_host_scalar(utils::type_to_string<ScalarType>::value(), binder_.get(NULL)));
          }

          /** @brief Scalar mapping */
          template<class ScalarType>
          result_type operator()(scalar<ScalarType> const & scal) const
          {
            return result_type(new mapped_scalar(utils::type_to_string<ScalarType>::value(), binder_.get(&viennacl::traits::handle(scal))));
          }

          /** @brief Vector mapping */
          template<class ScalarType>
          result_type operator()(vector_base<ScalarType> const & vec) const
          {
            return result_type(new mapped_vector(utils::type_to_string<ScalarType>::value(), binder_.get(&viennacl::traits::handle(vec))));
          }

          /** @brief Implicit vector mapping */
          template<class ScalarType>
          result_type operator()(implicit_vector_base<ScalarType> const & /*vec*/) const
          {
            return result_type(new mapped_implicit_vector(utils::type_to_string<ScalarType>::value(), binder_.get(NULL)));
          }

          /** @brief Matrix mapping */
          template<class ScalarType>
          result_type operator()(matrix_base<ScalarType> const & mat) const
          {
            return result_type(new mapped_matrix(utils::type_to_string<ScalarType>::value(), binder_.get(&viennacl::traits::handle(mat)),
                                                  viennacl::traits::row_major(mat)));
          }

          /** @brief Implicit matrix mapping */
          template<class ScalarType>
          result_type operator()(implicit_matrix_base<ScalarType> const & /*mat*/) const
          {
            return result_type(new mapped_implicit_matrix(utils::type_to_string<ScalarType>::value(), binder_.get(NULL)));
          }

          /** @brief Traversal functor */
          void operator()(scheduler::statement const & statement, vcl_size_t root_idx, leaf_t leaf_t) const {
            mapping_type::key_type key(root_idx, leaf_t);
            scheduler::statement_node const & root_node = statement.array()[root_idx];

            if(leaf_t == LHS_NODE_TYPE && root_node.lhs.type_family != scheduler::COMPOSITE_OPERATION_FAMILY)
                 mapping_.insert(mapping_type::value_type(key, utils::call_on_element(root_node.lhs, *this)));
            else if(leaf_t == RHS_NODE_TYPE && root_node.rhs.type_family != scheduler::COMPOSITE_OPERATION_FAMILY)
                 mapping_.insert(mapping_type::value_type(key,  utils::call_on_element(root_node.rhs, *this)));
            else if( leaf_t== PARENT_NODE_TYPE)
            {
                if(root_node.op.type==scheduler::OPERATION_BINARY_VECTOR_DIAG_TYPE)
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_vector_diag>(&statement, root_idx, &mapping_)));
                else if(root_node.op.type==scheduler::OPERATION_BINARY_MATRIX_DIAG_TYPE)
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_matrix_diag>(&statement, root_idx, &mapping_)));
                else if(root_node.op.type==scheduler::OPERATION_BINARY_MATRIX_ROW_TYPE)
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_matrix_row>(&statement, root_idx, &mapping_)));
                else if(root_node.op.type==scheduler::OPERATION_BINARY_MATRIX_COLUMN_TYPE)
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_matrix_column>(&statement, root_idx, &mapping_)));
                else if(is_scalar_reduction(root_node))
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_scalar_reduction>(&statement, root_idx, &mapping_)));
                else if(is_vector_reduction(root_node))
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_row_wise_reduction>(&statement, root_idx, &mapping_)));
                else if(root_node.op.type == scheduler::OPERATION_BINARY_MAT_MAT_PROD_TYPE)
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_matrix_product>(&statement, root_idx, &mapping_)));
                else if(root_node.op.type == scheduler::OPERATION_UNARY_TRANS_TYPE)
                  mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_trans>(&statement, root_idx, &mapping_)));
           }
          }

        private:
          symbolic_binder & binder_;
          mapping_type & mapping_;
      };

      /** @brief functor for generating the prototype of a statement */
      class prototype_generation_traversal : public tree_parsing::traversal_functor
      {
        private:
          std::set<std::string> & already_generated_;
          std::string & str_;
          mapping_type const & mapping_;
        public:
          prototype_generation_traversal(std::set<std::string> & already_generated, std::string & str, mapping_type const & mapping) : already_generated_(already_generated), str_(str),  mapping_(mapping){ }

          void operator()(scheduler::statement const & statement, vcl_size_t root_idx, leaf_t leaf) const
          {
              scheduler::statement_node const & root_node = statement.array()[root_idx];
              if( (leaf==LHS_NODE_TYPE && root_node.lhs.type_family!=scheduler::COMPOSITE_OPERATION_FAMILY)
                ||(leaf==RHS_NODE_TYPE && root_node.rhs.type_family!=scheduler::COMPOSITE_OPERATION_FAMILY) )
              {
                mapped_object * obj = mapping_.at(std::make_pair(root_idx,leaf)).get();
                obj->append_kernel_arguments(already_generated_, str_);
              }
          }
      };



      /** @brief functor for setting the arguments of a kernel */
      class set_arguments_functor : public tree_parsing::traversal_functor
      {
        public:
          typedef void result_type;

          set_arguments_functor(symbolic_binder & binder, unsigned int & current_arg, viennacl::ocl::kernel & kernel) : binder_(binder), current_arg_(current_arg), kernel_(kernel){ }

          template<class ScalarType>
          result_type operator()(ScalarType const & scal) const {
            typedef typename viennacl::result_of::cl_type<ScalarType>::type cl_scalartype;
            kernel_.arg(current_arg_++, cl_scalartype(scal));
          }

          /** @brief Scalar mapping */
          template<class ScalarType>
          result_type operator()(scalar<ScalarType> const & scal) const {
            if(binder_.bind(&viennacl::traits::handle(scal)))
              kernel_.arg(current_arg_++, scal.handle().opencl_handle());
          }

          /** @brief Vector mapping */
          template<class ScalarType>
          result_type operator()(vector_base<ScalarType> const & vec) const {
            if(binder_.bind(&viennacl::traits::handle(vec)))
            {
              kernel_.arg(current_arg_++, vec.handle().opencl_handle());
              kernel_.arg(current_arg_++, cl_uint(viennacl::traits::start(vec)));
              kernel_.arg(current_arg_++, cl_uint(viennacl::traits::stride(vec)));
            }
          }

          /** @brief Implicit vector mapping */
          template<class ScalarType>
          result_type operator()(implicit_vector_base<ScalarType> const & vec) const
          {
            typedef typename viennacl::result_of::cl_type<ScalarType>::type cl_scalartype;
            kernel_.arg(current_arg_++, cl_scalartype(vec.value()));
            if(vec.has_index())
              kernel_.arg(current_arg_++, cl_uint(vec.index()));
          }

          /** @brief Matrix mapping */
          template<class ScalarType>
          result_type operator()(matrix_base<ScalarType> const & mat) const
          {
            if(binder_.bind(&viennacl::traits::handle(mat)))
            {
              kernel_.arg(current_arg_++, mat.handle().opencl_handle());
              kernel_.arg(current_arg_++, cl_uint(viennacl::traits::ld(mat)));
              if(mat.row_major())
              {
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::start2(mat)));
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::start1(mat)));
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::stride2(mat)));
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::stride1(mat)));
              }
              else
              {
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::start1(mat)));
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::start2(mat)));
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::stride1(mat)));
                kernel_.arg(current_arg_++, cl_uint(viennacl::traits::stride2(mat)));
              }
            }
          }

          /** @brief Implicit matrix mapping */
          template<class ScalarType>
          result_type operator()(implicit_matrix_base<ScalarType> const & mat) const
          {
            kernel_.arg(current_arg_++, typename viennacl::result_of::cl_type<ScalarType>::type(mat.value()));
          }

          /** @brief Traversal functor: */
          void operator()(scheduler::statement const & statement, vcl_size_t root_idx, leaf_t leaf_t) const
          {
            scheduler::statement_node const & root_node = statement.array()[root_idx];
            if(leaf_t==LHS_NODE_TYPE && root_node.lhs.type_family != scheduler::COMPOSITE_OPERATION_FAMILY)
              utils::call_on_element(root_node.lhs, *this);
            else if(leaf_t==RHS_NODE_TYPE && root_node.rhs.type_family != scheduler::COMPOSITE_OPERATION_FAMILY)
              utils::call_on_element(root_node.rhs, *this);
          }

        private:
          symbolic_binder & binder_;
          unsigned int & current_arg_;
          viennacl::ocl::kernel & kernel_;
      };

    protected:

      static void generate_prototype(utils::kernel_generation_stream & stream, std::string const & name, std::string const & first_arguments, std::vector<mapping_type> const & mappings, statements_container statements)
      {
        statements_container::data_type::const_iterator sit;
        std::vector<mapping_type>::const_iterator mit;
        std::set<std::string> already_generated;

        std::string arguments = first_arguments;
        for(mit = mappings.begin(), sit = statements.data().begin() ; sit != statements.data().end() ; ++sit, ++mit)
          tree_parsing::traverse(*sit, sit->root(), prototype_generation_traversal(already_generated, arguments, *mit), true);
        arguments.erase(arguments.size()-1); //Last comma pruned
        stream << "__kernel " << "void " << name << "(" << arguments << ")" << std::endl;
      }

      void set_arguments(statements_container const & statements, viennacl::ocl::kernel & kernel, unsigned int & current_arg)
      {
        tools::shared_ptr<symbolic_binder> binder = make_binder(binding_policy_);
        for(statements_container::data_type::const_iterator itt = statements.data().begin() ; itt != statements.data().end() ; ++itt)
          tree_parsing::traverse(*itt, itt->root(), set_arguments_functor(*binder,current_arg,kernel), true);
      }

      class invalid_template_exception : public std::exception
      {
      public:
        invalid_template_exception() : message_() {}
        invalid_template_exception(std::string message) :
          message_("ViennaCL: Internal error: The generator cannot apply the given template to the given statement: " + message + "\n"
                   "If you are using a builtin template, please report on viennacl-support@lists.sourceforge.net! We will provide a fix as soon as possible\n"
                   "If you are using your own template, please try using other parameters") {}
        virtual const char* what() const throw() { return message_.c_str(); }
        virtual ~invalid_template_exception() throw() {}
      private:
        std::string message_;
      };

      static void fetching_loop_info(fetching_policy_type policy, std::string const & bound, utils::kernel_generation_stream & stream, std::string & init, std::string & upper_bound, std::string & inc, std::string const & domain_id, std::string const & domain_size)
      {
        if(policy==FETCH_FROM_GLOBAL_STRIDED)
        {
          init = domain_id;
          upper_bound = bound;
          inc = domain_size;
        }
        else if(policy==FETCH_FROM_GLOBAL_CONTIGUOUS)
        {
          std::string chunk_size = "chunk_size";
          std::string chunk_start = "chunk_start";
          std::string chunk_end = "chunk_end";

          stream << "unsigned int " << chunk_size << " = (" << domain_size << "-1)/" << domain_size << ";" << std::endl;
          stream << "unsigned int " << chunk_start << " =" << domain_id << "*" << chunk_size << ";" << std::endl;
          stream << "unsigned int " << chunk_end << " = min(" << chunk_start << "+" << chunk_size << ", " << bound << ");" << std::endl;
          init = chunk_start;
          upper_bound = chunk_end;
          inc = "1";
        }
      }

      static bool is_node_trans(scheduler::statement::container_type const & array, size_t root_idx, leaf_t leaf_type)
      {
        bool res = false;
        scheduler::lhs_rhs_element scheduler::statement_node::*ptr;
        if(leaf_type==LHS_NODE_TYPE)
          ptr = &scheduler::statement_node::lhs;
        else
          ptr = &scheduler::statement_node::rhs;
        scheduler::statement_node const * node = &array[root_idx];
        while((node->*ptr).type_family==scheduler::COMPOSITE_OPERATION_FAMILY)
        {
          if(array[(node->*ptr).node_index].op.type==scheduler::OPERATION_UNARY_TRANS_TYPE)
            res = !res;
          node = &array[(node->*ptr).node_index];
        }
        return res;
      }

    public:

      struct parameters_type
      {
        parameters_type(unsigned int _simd_width, unsigned int _local_size_1, unsigned int _local_size_2, unsigned int _num_kernels) : simd_width(_simd_width), local_size_0(_local_size_1), local_size_1(_local_size_2), num_kernels(_num_kernels){ }

        unsigned int simd_width;
        unsigned int local_size_0;
        unsigned int local_size_1;
        unsigned int num_kernels;
      };

    private:

      virtual int check_invalid_impl(viennacl::ocl::device const & /*dev*/) const { return TEMPLATE_VALID; }
      virtual unsigned int n_lmem_elements() const { return 0; }

      /** @brief Generates the body of the associated kernel function */
      virtual std::vector<std::string> generate_impl(std::string const & kernel_prefix, statements_container const & statements, std::vector<mapping_type> const & mapping) const = 0;

    protected:

      static bool has_strided_access(statements_container const & statements)
      {
        for(statements_container::data_type::const_iterator it = statements.data().begin() ; it != statements.data().end() ; ++it)
        {
          //checks for vectors
          std::vector<scheduler::lhs_rhs_element> vectors;
          tree_parsing::traverse(*it, it->root(), tree_parsing::filter_elements(scheduler::DENSE_VECTOR_TYPE, vectors), true);
          for(std::vector<scheduler::lhs_rhs_element>::iterator itt = vectors.begin() ; itt != vectors.end() ; ++itt)
            if(utils::call_on_vector(*itt, utils::stride_fun())>1)
              return true;

          //checks for matrix
          std::vector<scheduler::lhs_rhs_element> matrices;
          tree_parsing::traverse(*it, it->root(), tree_parsing::filter_elements(scheduler::DENSE_MATRIX_TYPE, matrices), true);
          for(std::vector<scheduler::lhs_rhs_element>::iterator itt = matrices.begin() ; itt != matrices.end() ; ++itt)
            if(utils::call_on_matrix(*itt, utils::stride1_fun())>1 || utils::call_on_matrix(*itt, utils::stride2_fun())>2)
              return true;
        }
        return false;
      }

      static vcl_size_t vector_size(scheduler::statement_node const & node, bool up_to_internal_size)
      {
        using namespace scheduler;
        using namespace utils;
        if(node.op.type==OPERATION_BINARY_MATRIX_DIAG_TYPE)
        {
          vcl_size_t size1 = up_to_internal_size?call_on_matrix(node.lhs, internal_size1_fun()):call_on_matrix(node.lhs, size1_fun());
          vcl_size_t size2 = up_to_internal_size?call_on_matrix(node.lhs, internal_size2_fun()):call_on_matrix(node.lhs, size2_fun());
          return std::min<vcl_size_t>(size1, size2);
        }
        else if(node.op.type==OPERATION_BINARY_MATRIX_ROW_TYPE)
          return up_to_internal_size?call_on_matrix(node.lhs, internal_size2_fun()):call_on_matrix(node.lhs, size2_fun());
        else if(node.op.type==OPERATION_BINARY_MATRIX_COLUMN_TYPE)
          return up_to_internal_size?call_on_matrix(node.lhs, internal_size1_fun()):call_on_matrix(node.lhs, size1_fun());
        else
          return up_to_internal_size?call_on_vector(node.lhs, internal_size_fun()):call_on_vector(node.lhs, size_fun());
      }

      //NB : templates are not used here because declaring a functor out of the generate() functions would be harder to read
      struct loop_body_base
      {
        virtual void operator()(utils::kernel_generation_stream & stream, unsigned int simd_width) const = 0;
      };
      static void element_wise_loop_1D(utils::kernel_generation_stream & stream, loop_body_base const & loop_body,
                       fetching_policy_type fetch, unsigned int simd_width, std::string const & i, std::string const & bound, std::string const & domain_id, std::string const & domain_size)
      {
        std::string strwidth = tools::to_string(simd_width);
        std::string boundround = bound + "/" + strwidth;

        std::string init, upper_bound, inc;
        fetching_loop_info(fetch, boundround, stream, init, upper_bound, inc, domain_id, domain_size);
        stream << "for(unsigned int " << i << " = " << init << "; " << i << " < " << upper_bound << " ; " << i << " += " << inc << ")" << std::endl;
        stream << "{" << std::endl;
        stream.inc_tab();
        loop_body(stream, simd_width);
        stream.dec_tab();
        stream << "}" << std::endl;

        if(simd_width>1)
        {
          stream << "for(unsigned int " << i << " = " << boundround << "*" << strwidth << " + " << domain_id << " ; " << i << " < " << bound << "; " << i << " += " + domain_size + ")" << std::endl;
          stream << "{" << std::endl;
          stream.inc_tab();
          loop_body(stream, 1);
          stream.dec_tab();
          stream << "}" << std::endl;
        }
      }

    public:
      /** @brief The constructor */
      template_base(template_base::parameters_type const & parameters, binding_policy_t binding_policy) : p_(parameters), binding_policy_(binding_policy){ }
      virtual ~template_base(){ }

      /** @brief returns whether or not the profile has undefined behavior on particular device */
      int check_invalid(statements_container const & statements, viennacl::ocl::device const & device) const
      {
        using namespace viennacl::tools;

        scheduler::statement const & statement = statements.data().front();
        unsigned int scalartype_size = utils::size_of(lhs_most(statement.array(), statement.root()).lhs.numeric_type);

        //Query device informations
        size_t lmem_available = static_cast<size_t>(device.local_mem_size());
        size_t lmem_usage = scalartype_size*n_lmem_elements();
        if(lmem_usage>lmem_available)
          return TEMPLATE_LOCAL_MEMORY_OVERFLOW;

        //Invalid work group size
        size_t max_workgroup_size = device.max_work_group_size();
        std::vector<size_t> max_work_item_sizes = device.max_work_item_sizes();
        if(p_.local_size_0*p_.local_size_1 > max_workgroup_size)
          return TEMPLATE_WORK_GROUP_SIZE_OVERFLOW;
        if(p_.local_size_0 > max_work_item_sizes[0])
          return TEMPLATE_LOCAL_SIZE_0_OVERFLOW;

        if(p_.local_size_1 > max_work_item_sizes[1])
          return TEMPLATE_LOCAL_SIZE_1_OVERFLOW;

        //Advice from the Intel guide
        unsigned int warp_size = 8;
        if(device.type()==CL_DEVICE_TYPE_GPU)
        {
          //Advice from the nvidia guide
          warp_size = 32;
          //Advice from the AMD guide
          if(device.vendor_id()==4098)
            warp_size = 64;
        }
        if(((p_.local_size_0*p_.local_size_1)%warp_size)>0)
          return TEMPLATE_LOCAL_SIZE_NOT_WARP_MULTIPLE;
          
        //Invalid SIMD Width
        if(p_.simd_width!=1 && p_.simd_width!=2 &&
                    p_.simd_width!=4 && p_.simd_width!=8 &&
                    p_.simd_width!=16)
          return TEMPLATE_INVALID_SIMD_WIDTH;

        return check_invalid_impl(device);
      }



      std::vector<std::string> generate(std::string const & kernel_prefix, statements_container const & statements, viennacl::ocl::device const & /*device*/)
      {
        statements_container::data_type::const_iterator sit;
        std::vector<mapping_type>::iterator mit;

        //Create mapping
        std::vector<mapping_type> mappings(statements.data().size());
        tools::shared_ptr<symbolic_binder> binder = make_binder(binding_policy_);
        for(mit = mappings.begin(), sit = statements.data().begin() ; sit != statements.data().end() ; ++sit, ++mit)
          tree_parsing::traverse(*sit, sit->root(), map_functor(*binder,*mit), true);

        return generate_impl(kernel_prefix, statements, mappings);
      }

      virtual void enqueue(std::string const & kernel_prefix, std::vector<lazy_program_compiler> & programs, statements_container const & statements) = 0;

    protected:
      template_base::parameters_type const & p_;
      binding_policy_t binding_policy_;
    };

  }

}

#endif
