#include <iostream>
#include <fstream>

#include <cstring>
#include <cmath>

#include <unistd.h>

#include "core/mesh.hpp"
#include "core/meshers.hpp"
#include "core/quadratures.hpp"
#include "core/bases.hpp"
#include "core/solvers.hpp"
#include "core/blaze_sparse_init.hpp"
#include "core/dataio.hpp"


template<typename Mesh>
class assembler
{
    using T = typename Mesh::coordinate_type;
    using triplet_type = blaze::triplet<T>;

    std::vector<triplet_type>       triplets;
    std::vector<triplet_type>       pc_triplets;

    size_t                          sys_size, basis_size;
    bool                            build_pc;

public:
    blaze::CompressedMatrix<T>      lhs;
    blaze::DynamicVector<T>         rhs;
    blaze::CompressedMatrix<T>      pc;
    blaze::DynamicVector<T>         pc_temp;

    assembler()
        : sys_size(0), basis_size(0)
    {}

    assembler(const Mesh& msh, size_t degree, bool bpc = false)
    {
        initialize(msh, degree, bpc);
    }

    void initialize(const Mesh& msh, size_t degree, bool bpc = false)
    {
        basis_size = dg2d::bases::scalar_basis_size(degree,2);
        sys_size = basis_size * msh.cells.size();

        lhs.resize( sys_size, sys_size );
        rhs.resize( sys_size );
        pc.resize( sys_size, sys_size );
        pc_temp = blaze::DynamicVector<T>(sys_size, 0.0);
        build_pc = true;
    }

    bool assemble(const Mesh& msh,
                  const typename Mesh::cell_type& cl_a,
                  const typename Mesh::cell_type& cl_b,
                  const blaze::DynamicMatrix<T>& local_rhs)
    {
        if ( sys_size == 0 or basis_size == 0 )
            throw std::invalid_argument("Assembler in invalid state");

        auto cl_a_ofs = offset(msh, cl_a) * basis_size;
        auto cl_b_ofs = offset(msh, cl_b) * basis_size;

        for (size_t i = 0; i < basis_size; i++)
        {
            auto ci = cl_a_ofs + i;

            for (size_t j = 0; j < basis_size; j++)
            {
                auto cj = cl_b_ofs + j;

                triplets.push_back( {ci, cj, local_rhs(i,j)} );

                if (build_pc && ci == cj)
                    pc_temp[ci] += local_rhs(i,j);
            }
        }
        
        return true;
    }

    bool assemble(const Mesh& msh, const typename Mesh::cell_type& cl,
                  const blaze::DynamicMatrix<T>& local_rhs,
                  const blaze::DynamicVector<T>& local_lhs)
    {
        if ( sys_size == 0 or basis_size == 0 )
            throw std::invalid_argument("Assembler in invalid state");

        auto cl_ofs = offset(msh, cl) * basis_size;

        for (size_t i = 0; i < basis_size; i++)
        {
            auto ci = cl_ofs + i;

            for (size_t j = 0; j < basis_size; j++)
            {
                auto cj = cl_ofs + j;

                triplets.push_back( {ci, cj, local_rhs(i,j)} );

                if (build_pc && ci == cj)
                    pc_temp[ci] += local_rhs(i,j);
            }

            rhs[ci] = local_lhs[i];
        }
        
        return true;
    }

    void finalize()
    {
        if ( sys_size == 0 or basis_size == 0 )
            throw std::invalid_argument("Assembler in invalid state");

        blaze::init_from_triplets(lhs, triplets.begin(), triplets.end());
        triplets.clear();

        if (build_pc)
        {
            for(size_t i = 0; i < pc_temp.size(); i++)
            {
                assert( std::abs(pc_temp[i]) > 1e-2 );
                pc_triplets.push_back({i,i,1.0/pc_temp[i]});
            }

            blaze::init_from_triplets(pc, pc_triplets.begin(), pc_triplets.end());
            pc_triplets.clear();
        }
    }

    size_t system_size() const { return sys_size; }
};

enum class solver_type
{
    CG,
    BICGSTAB,
    QMR,
    DIRECT
};

template<typename T>
struct dg_config
{
    T               eta;
    int             degree;
    int             ref_levels;
    bool            use_preconditioner;
    bool            shatter;
    solver_type     solver;


    dg_config()
        : eta(1.0), degree(1), ref_levels(4), use_preconditioner(false),
          solver(solver_type::BICGSTAB)
    {}
};


namespace params {
/* Reaction term coefficient */
template<typename T>
T
mu(const point<T,2>& pt)
{
    return 1.0;
}

/* Advection term coefficient */
template<typename T>
blaze::StaticVector<T, 2>
beta(const point<T,2>& pt)
{
    blaze::StaticVector<T,2> ret;
    ret[0] = 1.0;
    ret[1] = 0.0;
    return ret;
}

/* Diffusion term coefficient */
template<typename T>
T
epsilon(const point<T,2>& pt)
{
    return 1.0;
}

} // namespace params

namespace data {
template<typename T>
T
rhs(const point<T,2>& pt)
{
    auto sx = std::sin(M_PI*pt.x());
    auto sy = std::sin(M_PI*pt.y());

    return 2.0 * M_PI * M_PI * sx * sy;
};

template<typename T>
T
dirichlet(const point<T,2>& pt)
{
    return 0.0;
};

template<typename T>
T
diffusion_ref_sol(const point<T,2>& pt)
{
    auto sx = std::sin(M_PI*pt.x());
    auto sy = std::sin(M_PI*pt.y());

    return sx * sy;
};

template<typename T>
T
adv_rhs(const point<T,2>& pt)
{
    auto u = std::sin( M_PI * pt.x() );
    auto du_x = M_PI * std::cos( M_PI * pt.x() );
    auto du_y = 0.0;

    blaze::StaticVector<T,2> du({du_x, du_y});

    return dot(params::beta(pt), du) + params::mu(pt)*u;
};

template<typename T>
T
adv_ref_sol(const point<T,2>& pt)
{
    return std::sin( M_PI * pt.x() );
};

} // namespace data




template<typename T>
struct solver_status
{
    T   mesh_h;
    T   L2_errsq_qp;
    T   L2_errsq_mm;
};

template<typename T>
std::ostream& operator<<(std::ostream& os, const solver_status<T>& s)
{
    os << s.mesh_h << " " << s.L2_errsq_qp << " " << s.L2_errsq_mm;
    return os;
}

template<typename Mesh>
solver_status<typename Mesh::coordinate_type>
run_diffusion_solver(Mesh& msh, const dg_config<typename Mesh::coordinate_type>& cfg)
{
    using mesh_type = Mesh;
    using T = typename mesh_type::coordinate_type;

    solver_status<T>    status;
    status.mesh_h = diameter(msh);

    msh.compute_connectivity();

    size_t degree = cfg.degree;
    T eta = 3*degree*degree*cfg.eta;


    /* PROBLEM ASSEMBLY */
    assembler<mesh_type> assm(msh, degree, cfg.use_preconditioner);

    for (auto& tcl : msh.cells)
    {
        auto qps = dg2d::quadratures::integrate(msh, tcl, 2*degree);
        auto tbasis = dg2d::bases::make_basis(msh, tcl, degree);

        blaze::DynamicMatrix<T> K(tbasis.size(), tbasis.size(), 0.0);
        blaze::DynamicVector<T> loc_rhs(tbasis.size(), 0.0);

        for (auto& qp : qps)
        {
            auto ep   = qp.point();
            auto phi  = tbasis.eval(ep);
            auto dphi = tbasis.eval_grads(ep);

            K += qp.weight() * dphi * trans(dphi);
            loc_rhs += qp.weight() * data::rhs(ep) * phi;
        }

        auto fcs = faces(msh, tcl);
        for (auto& fc : fcs)
        {
            blaze::DynamicMatrix<T> Att(tbasis.size(), tbasis.size(), 0.0);
            blaze::DynamicMatrix<T> Atn(tbasis.size(), tbasis.size(), 0.0);

            auto nv = neighbour_via(msh, tcl, fc);
            auto ncl = nv.first;
            auto nbasis = dg2d::bases::make_basis(msh, ncl, degree);
            assert(tbasis.size() == nbasis.size());

            auto n     = normal(msh, tcl, fc);
            auto eta_l = eta / diameter(msh, fc);
            auto f_qps = dg2d::quadratures::integrate(msh, fc, 2*degree);
            
            for (auto& fqp : f_qps)
            {
                auto ep     = fqp.point();
                auto tphi   = tbasis.eval(ep);
                auto tdphi  = tbasis.eval_grads(ep);

                if (nv.second)
                {   /* NOT on a boundary */
                    Att += + fqp.weight() * eta_l * tphi * trans(tphi);
                    Att += - fqp.weight() * 0.5 * tphi * trans(tdphi*n);
                    Att += - fqp.weight() * 0.5 * (tdphi*n) * trans(tphi);
                }
                else
                {   /* On a boundary*/
                    Att += + fqp.weight() * eta_l * tphi * trans(tphi);
                    Att += - fqp.weight() * tphi * trans(tdphi*n);
                    Att += - fqp.weight() * (tdphi*n) * trans(tphi);

                    loc_rhs -= fqp.weight() * data::dirichlet(ep) * (tdphi*n);
                    loc_rhs += fqp.weight() * eta_l * data::dirichlet(ep) * tphi;
                    continue;
                }

                auto nphi   = nbasis.eval(ep);
                auto ndphi  = nbasis.eval_grads(ep);

                Atn += - fqp.weight() * eta_l * tphi * trans(nphi);
                Atn += - fqp.weight() * 0.5 * tphi * trans(ndphi*n);
                Atn += + fqp.weight() * 0.5 * (tdphi*n) * trans(nphi);
            }

            assm.assemble(msh, tcl, tcl, Att);
            if (nv.second)
                assm.assemble(msh, tcl, ncl, Atn);
        }

        assm.assemble(msh, tcl, K, loc_rhs);
    }

    assm.finalize();

    /* SOLUTION PART */
    blaze::DynamicVector<T> sol(assm.system_size());

    conjugated_gradient_params<T> cgp;
    cgp.verbose = true;
    cgp.rr_max = 10000;
    cgp.rr_tol = 1e-8;
    cgp.max_iter = 2*assm.system_size();

    conjugated_gradient(cgp, assm.lhs, assm.rhs, sol);

    /* POSTPROCESS PART */
    status.L2_errsq_qp = 0.0;
    status.L2_errsq_mm = 0.0;
    for (auto& cl : msh.cells)
    {
        auto basis = dg2d::bases::make_basis(msh, cl, degree);
        auto basis_size = basis.size();
        auto ofs = offset(msh, cl);

        blaze::DynamicVector<T> loc_sol(basis_size);
        for (size_t i = 0; i < basis_size; i++)
            loc_sol[i] = sol[basis_size * ofs + i];

        blaze::DynamicMatrix<T> M(basis_size, basis_size, 0.0);
        blaze::DynamicVector<T> a(basis_size, 0.0);

        auto qps = dg2d::quadratures::integrate(msh, cl, 2*degree);
        for (auto& qp : qps)
        {
            auto ep   = qp.point();
            auto phi  = basis.eval(ep);

            auto sv = data::diffusion_ref_sol(ep);

            M += qp.weight() * phi * trans(phi);
            a += qp.weight() * sv * phi;

            T cv = dot(loc_sol, phi);
            status.L2_errsq_qp += qp.weight() * (sv - cv) * (sv - cv);
        }

        blaze::DynamicVector<T> proj = blaze::solve_LU(M, a);
        status.L2_errsq_mm += dot(proj-loc_sol, M*(proj-loc_sol));

    }

#ifdef WITH_SILO

    blaze::DynamicVector<T> var(msh.cells.size());
    auto bs = dg2d::bases::scalar_basis_size(degree, 2);
    for (size_t i = 0; i < msh.cells.size(); i++)
    {
        var[i] = sol[bs*i];
    }


    blaze::DynamicVector<T> dbg_mu( msh.points.size() );
    blaze::DynamicVector<T> dbg_epsilon( msh.points.size() );
    blaze::DynamicVector<T> dbg_beta_x( msh.points.size() );
    blaze::DynamicVector<T> dbg_beta_y( msh.points.size() );

    for (size_t i = 0; i < msh.points.size(); i++)
    {
        auto pt = msh.points.at(i);
        dbg_mu[i] = params::mu(pt);
        dbg_epsilon[i] = params::epsilon(pt);
        auto beta_pt = params::beta(pt);
        dbg_beta_x[i] = beta_pt[0];
        dbg_beta_y[i] = beta_pt[1];
    }

    dg2d::silo_database silo;
    silo.create("test_dg.silo");

    silo.add_mesh(msh, "test_mesh");

    silo.add_zonal_variable("test_mesh", "solution", var);

    silo.add_nodal_variable("test_mesh", "mu", dbg_mu);
    silo.add_nodal_variable("test_mesh", "epsilon", dbg_epsilon);
    silo.add_nodal_variable("test_mesh", "beta_x", dbg_beta_x);
    silo.add_nodal_variable("test_mesh", "beta_y", dbg_beta_y);
#endif

    return status;
}




template<typename Mesh>
solver_status<typename Mesh::coordinate_type>
run_advection_reaction_solver(Mesh& msh, const dg_config<typename Mesh::coordinate_type>& cfg)
{
    using mesh_type = Mesh;
    using T = typename mesh_type::coordinate_type;

    solver_status<T>    status;
    status.mesh_h = diameter(msh);

    msh.compute_connectivity();

    size_t degree = cfg.degree;

    assembler<mesh_type> assm(msh, degree, cfg.use_preconditioner);
    for (auto& tcl : msh.cells)
    {
        auto qps = dg2d::quadratures::integrate(msh, tcl, 2*degree);
        auto tbasis = dg2d::bases::make_basis(msh, tcl, degree);

        blaze::DynamicMatrix<T> K(tbasis.size(), tbasis.size(), 0.0);
        blaze::DynamicVector<T> loc_rhs(tbasis.size(), 0.0);

        for (auto& qp : qps)
        {
            auto ep   = qp.point();
            auto phi  = tbasis.eval(ep);
            auto dphi = tbasis.eval_grads(ep);

            /* Reaction */
            K += params::mu(ep) * qp.weight() * phi * trans(phi);
            /* Advection */
            K += qp.weight() * phi * trans( dphi*params::beta(ep) );

            loc_rhs += qp.weight() * data::adv_rhs(ep) * phi;
        }

        auto fcs = faces(msh, tcl);
        for (auto& fc : fcs)
        {
            blaze::DynamicMatrix<T> Att(tbasis.size(), tbasis.size(), 0.0);
            blaze::DynamicMatrix<T> Atn(tbasis.size(), tbasis.size(), 0.0);

            auto nv = neighbour_via(msh, tcl, fc);
            auto ncl = nv.first;
            auto nbasis = dg2d::bases::make_basis(msh, ncl, degree);
            assert(tbasis.size() == nbasis.size());

            auto n     = normal(msh, tcl, fc);
            auto f_qps = dg2d::quadratures::integrate(msh, fc, 2*degree);
            
            for (auto& fqp : f_qps)
            {
                auto ep     = fqp.point();
                auto tphi   = tbasis.eval(ep);
                auto tdphi  = tbasis.eval_grads(ep);

                if (nv.second)
                {   /* NOT on a boundary */
                    /* Advection-Reaction */
                    T beta_nf = dot(params::beta(ep), n);
                    auto coeff = beta_nf;// - eta*beta_nf/2.0;
                    Att += - fqp.weight() * 0.5*coeff * tphi * trans(tphi);
                }
                else
                {   /* On a boundary*/
                    T beta_nf = dot(params::beta(ep), n);
                    auto beta_minus = 0.5*(std::abs(beta_nf) - beta_nf);

                    if (beta_nf < 0.0)
                    {
                        Att += fqp.weight() * beta_minus * tphi * trans(tphi);
                    }

                    continue;
                }

                auto nphi   = nbasis.eval(ep);
                auto ndphi  = nbasis.eval_grads(ep);

                /* Advection-Reaction */
                auto beta_nf = dot(params::beta(ep),n);
                auto coeff = beta_nf;// - eta*beta_nf/2.0;
                Atn += + fqp.weight() * coeff * 0.5 * tphi * trans(nphi);
            }

            assm.assemble(msh, tcl, tcl, Att);
            if (nv.second)
                assm.assemble(msh, tcl, ncl, Atn);
        }

        assm.assemble(msh, tcl, K, loc_rhs);
    }

    assm.finalize();

    /* SOLUTION PART */
    blaze::DynamicVector<T> sol(assm.system_size());

    conjugated_gradient_params<T> cgp;
    cgp.verbose = true;
    cgp.rr_max = 10000;
    cgp.rr_tol = 1e-8;
    cgp.max_iter = 2*assm.system_size();
    cgp.use_normal_eqns = true; /* PAY ATTENTION TO THIS */

    conjugated_gradient(cgp, assm.lhs, assm.rhs, sol);

    status.L2_errsq_qp = 0.0;
    status.L2_errsq_mm = 0.0;
    for (auto& cl : msh.cells)
    {
        auto basis = dg2d::bases::make_basis(msh, cl, degree);
        auto basis_size = basis.size();
        auto ofs = offset(msh, cl);

        blaze::DynamicVector<T> loc_sol(basis_size);
        for (size_t i = 0; i < basis_size; i++)
            loc_sol[i] = sol[basis_size * ofs + i];

        blaze::DynamicMatrix<T> M(basis_size, basis_size, 0.0);
        blaze::DynamicVector<T> a(basis_size, 0.0);

        auto qps = dg2d::quadratures::integrate(msh, cl, 2*degree);
        for (auto& qp : qps)
        {
            auto ep   = qp.point();
            auto phi  = basis.eval(ep);

            auto sv = data::adv_ref_sol(ep);

            M += qp.weight() * phi * trans(phi);
            a += qp.weight() * sv * phi;

            T cv = dot(loc_sol, phi);
            status.L2_errsq_qp += qp.weight() * (sv - cv) * (sv - cv);
        }

        blaze::DynamicVector<T> proj = blaze::solve_LU(M, a);
        status.L2_errsq_mm += dot(proj-loc_sol, M*(proj-loc_sol));

    }


#ifdef WITH_SILO
    blaze::DynamicVector<T> var(msh.cells.size());
    auto bs = dg2d::bases::scalar_basis_size(degree, 2);
    for (size_t i = 0; i < msh.cells.size(); i++)
    {
        var[i] = sol[bs*i];
    }

    blaze::DynamicVector<T> dbg_mu( msh.points.size() );
    blaze::DynamicVector<T> dbg_epsilon( msh.points.size() );
    blaze::DynamicVector<T> dbg_beta_x( msh.points.size() );
    blaze::DynamicVector<T> dbg_beta_y( msh.points.size() );

    for (size_t i = 0; i < msh.points.size(); i++)
    {
        auto pt = msh.points.at(i);
        dbg_mu[i] = params::mu(pt);
        dbg_epsilon[i] = params::epsilon(pt);
        auto beta_pt = params::beta(pt);
        dbg_beta_x[i] = beta_pt[0];
        dbg_beta_y[i] = beta_pt[1];
    }

    dg2d::silo_database silo;
    silo.create("test_dg.silo");

    silo.add_mesh(msh, "test_mesh");

    silo.add_zonal_variable("test_mesh", "solution", var);

    silo.add_nodal_variable("test_mesh", "mu", dbg_mu);
    silo.add_nodal_variable("test_mesh", "epsilon", dbg_epsilon);
    silo.add_nodal_variable("test_mesh", "beta_x", dbg_beta_x);
    silo.add_nodal_variable("test_mesh", "beta_y", dbg_beta_y);
#endif

    return status;
}

template<typename T>
void run_triangle_dg(const dg_config<T>& cfg)
{
    using mesh_type = dg2d::simplicial_mesh<T>;

    mesh_type msh;
    auto mesher = dg2d::get_mesher(msh);
    mesher.create_mesh(msh, cfg.ref_levels);

    if (cfg.shatter)
        shatter_mesh(msh, 0.2);

    auto status = run_advection_reaction_solver(msh, cfg);
    std::cout << status << std::endl;
}

template<typename T>
void run_quadrangle_dg(const dg_config<T>& cfg)
{
    using mesh_type = dg2d::quad_mesh<T>;

    mesh_type msh;
    auto mesher = dg2d::get_mesher(msh);
    mesher.create_mesh(msh, cfg.ref_levels);

    if (cfg.shatter)
        shatter_mesh(msh, 0.2);

    run_advection_reaction_solver(msh, cfg);
}

enum class meshtype  {
    TRIANGULAR,
    QUADRANGULAR,
    TETRAHEDRAL,
    HEXAHEDRAL
};

int main(int argc, char **argv)
{
    _MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() & ~_MM_MASK_INVALID);

    using T = double;

    meshtype mt = meshtype::TRIANGULAR;

    dg_config<T> cfg;

    int     ch;

    cfg.shatter = false;

    while ( (ch = getopt(argc, argv, "e:k:r:hqsvpS")) != -1 )
    {
        switch(ch)
        {
            case 'e':
                cfg.eta = atof(optarg);
                break;

            case 'k':
                cfg.degree = atoi(optarg);
                if (cfg.degree < 1)
                {
                    std::cout << "Degree must be positive. Falling back to 1." << std::endl;
                    cfg.degree = 1;
                }
                break;

            case 'r':
                cfg.ref_levels = atoi(optarg);
                if (cfg.ref_levels < 0)
                {
                    std::cout << "Degree must be positive. Falling back to 1." << std::endl;
                    cfg.ref_levels = 1;
                }
                break;

            case 'v':
                break;

            case 'q':
                mt = meshtype::QUADRANGULAR;
                break;

            case 's':
                mt = meshtype::TRIANGULAR;
                break;

            case 'p':
                cfg.use_preconditioner = true;
                break;

            case 'S':
                cfg.shatter = true;
                break;

            case 'h':
            case '?':
            default:
                std::cout << "wrong arguments" << std::endl;
                exit(1);
        }
    }

    argc -= optind;
    argv += optind;


    switch (mt)
    {
        case meshtype::TRIANGULAR:
            run_triangle_dg(cfg);
            break;

        case meshtype::QUADRANGULAR:
            run_quadrangle_dg(cfg);
            break;

        case meshtype::TETRAHEDRAL:
        case meshtype::HEXAHEDRAL:
            break;
    }


    return 0;
}

