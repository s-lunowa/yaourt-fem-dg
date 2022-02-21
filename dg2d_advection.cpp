/*
 * Yaourt-FEM-DG - Yet AnOther Useful Resource for Teaching FEM and DG.
 *
 * Matteo Cicuttin (C) 2019
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <fstream>
#include <sstream>

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

#include "methods/dg.hpp"

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

} // namespace params

namespace data {
template<typename T>
T
rhs(const point<T,2>& pt)
{
    auto u = std::sin( M_PI * pt.x() );
    auto du_x = M_PI * std::cos( M_PI * pt.x() );
    auto du_y = 0.0;

    blaze::StaticVector<T,2> du({du_x, du_y});

    return dot(params::beta(pt), du) + params::mu(pt)*u;
};

template<typename T>
T
advection_ref_sol(const point<T,2>& pt)
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
    os << "Convergence results: " << std::endl;
    os << "  mesh size (h):         " << s.mesh_h << std::endl;
    os << "  L2-norm error (qp):    " << std::sqrt(s.L2_errsq_qp) << std::endl;
    os << "  L2-norm error (mm):    " << std::sqrt(s.L2_errsq_mm);
    return os;
}

template<typename T>
struct dg_config
{
    T               eta;
    int             degree;
    int             ref_levels;
    bool            use_preconditioner;
    bool            shatter;
    bool            use_upwinding;


    dg_config()
        : eta(1.0), degree(1), ref_levels(4), use_preconditioner(false),
          shatter(false), use_upwinding(false)
    {}
};

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

    T eta = cfg.eta;

    assembler<mesh_type> assm(msh, degree, cfg.use_preconditioner);
    for (auto& tcl : msh.cells)
    {
        auto tbasis = yaourt::bases::make_basis(msh, tcl, degree);

        blaze::DynamicMatrix<T> K(tbasis.size(), tbasis.size(), 0.0);
        blaze::DynamicVector<T> loc_rhs(tbasis.size(), 0.0);

        auto qps = yaourt::quadratures::integrate(msh, tcl, 2*degree);
        for (auto& qp : qps)
        {
            auto ep   = qp.point();
            auto phi  = tbasis.eval(ep);
            auto dphi = tbasis.eval_grads(ep);

            /* Reaction */
            K += params::mu(ep) * qp.weight() * phi * trans(phi);
            /* Advection */
            K += qp.weight() * phi * trans( dphi*params::beta(ep) );

            loc_rhs += qp.weight() * data::rhs(ep) * phi;
        }

        auto fcs = faces(msh, tcl);
        for (auto& fc : fcs)
        {
            blaze::DynamicMatrix<T> Att(tbasis.size(), tbasis.size(), 0.0);
            blaze::DynamicMatrix<T> Atn(tbasis.size(), tbasis.size(), 0.0);

            auto [ncl, has_neighbour] = neighbour_via(msh, tcl, fc);
            auto nbasis = yaourt::bases::make_basis(msh, ncl, degree);
            assert(tbasis.size() == nbasis.size());

            auto n     = normal(msh, tcl, fc);
            auto f_qps = yaourt::quadratures::integrate(msh, fc, 2*degree);
            
            for (auto& fqp : f_qps)
            {
                auto ep     = fqp.point();
                auto tphi   = tbasis.eval(ep);
                auto tdphi  = tbasis.eval_grads(ep);

                T beta_nf = dot(params::beta(ep), n);
                T fi_coeff;

                if (cfg.use_upwinding)
                    fi_coeff = beta_nf - eta * std::abs(beta_nf);
                else
                    fi_coeff = beta_nf;

                if (has_neighbour)
                {   /* NOT on a boundary */
                    Att += - fqp.weight() * 0.5 * fi_coeff * tphi * trans(tphi);
                }
                else
                {   /* On a boundary*/
                    auto beta_minus = 0.5*(std::abs(beta_nf) - beta_nf);

                    if (beta_nf < 0.0)
                        Att += fqp.weight() * beta_minus * tphi * trans(tphi);

                    continue;
                }

                auto nphi   = nbasis.eval(ep);
                auto ndphi  = nbasis.eval_grads(ep);

                /* Advection-Reaction */
                Atn += + fqp.weight() * fi_coeff * 0.5 * tphi * trans(nphi);
            }

            assm.assemble(msh, tcl, tcl, Att);
            if (has_neighbour)
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
    cgp.use_normal_eqns = true; /* USE CG ON NORMAL EQUATIONS - ASYMMETRIC SYSTEM */

    conjugated_gradient(cgp, assm.lhs, assm.rhs, sol);

    std::ofstream gnuplot_output("advection_reaction_solution.txt");

    status.L2_errsq_qp = 0.0;
    status.L2_errsq_mm = 0.0;
    for (auto& cl : msh.cells)
    {
        auto basis = yaourt::bases::make_basis(msh, cl, degree);
        auto basis_size = basis.size();
        auto ofs = offset(msh, cl);

        blaze::DynamicVector<T> loc_sol(basis_size);
        for (size_t i = 0; i < basis_size; i++)
            loc_sol[i] = sol[basis_size * ofs + i];

        auto tps = yaourt::make_test_points(msh, cl, 6);
        for (auto& tp : tps)
        {
            auto phi = basis.eval(tp);
            T sval = dot(loc_sol, phi);

            gnuplot_output << tp.x() << " " << tp.y() << " " << sval << std::endl;
        }

        blaze::DynamicMatrix<T> M(basis_size, basis_size, 0.0);
        blaze::DynamicVector<T> a(basis_size, 0.0);

        auto qps = yaourt::quadratures::integrate(msh, cl, 2*degree);
        for (auto& qp : qps)
        {
            auto ep   = qp.point();
            auto phi  = basis.eval(ep);

            auto sv = data::advection_ref_sol(ep);

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
    auto bs = yaourt::bases::scalar_basis_size(degree, 2);
    for (size_t i = 0; i < msh.cells.size(); i++)
    {
        var[i] = sol[bs*i];
    }

    blaze::DynamicVector<T> dbg_mu( msh.points.size() );
    blaze::DynamicVector<T> dbg_beta_x( msh.points.size() );
    blaze::DynamicVector<T> dbg_beta_y( msh.points.size() );

    for (size_t i = 0; i < msh.points.size(); i++)
    {
        auto pt = msh.points.at(i);
        dbg_mu[i] = params::mu(pt);
        auto beta_pt = params::beta(pt);
        dbg_beta_x[i] = beta_pt[0];
        dbg_beta_y[i] = beta_pt[1];
    }

    yaourt::dataio::silo_database silo;
    silo.create("test_dg.silo");
    silo.add_mesh(msh, "test_mesh");
    silo.add_zonal_variable("test_mesh", "solution", var);
    silo.add_nodal_variable("test_mesh", "mu", dbg_mu);
    silo.add_nodal_variable("test_mesh", "beta_x", dbg_beta_x);
    silo.add_nodal_variable("test_mesh", "beta_y", dbg_beta_y);
    silo.close();
#endif

    return status;
}

template<typename Mesh>
void run_dg(const dg_config<typename Mesh::coordinate_type>& cfg)
{
    using mesh_type = Mesh;
    using T = typename Mesh::coordinate_type;

    mesh_type msh;

    auto mesher = yaourt::get_mesher(msh);
    mesher.create_mesh(msh, cfg.ref_levels);

    if (cfg.shatter)
        shatter_mesh(msh, 0.2);

    solver_status<T> status;

    std::cout << "Running dG advection-reaction solver" << std::endl;
    std::cout << "  degree: " << cfg.degree << ", eta: " << cfg.eta << std::endl;
    status = run_advection_reaction_solver(msh, cfg);
    std::cout << status << std::endl;
}

int main(int argc, char **argv)
{
    _MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() & ~_MM_MASK_INVALID);

    using T = double;

    auto mt = yaourt::meshtype::TRIANGULAR;

    dg_config<T> cfg;

    int     ch;

    while ( (ch = getopt(argc, argv, "e:k:r:m:pSuh")) != -1 )
    {
        switch(ch)
        {
            case 'e':
                cfg.eta = atof(optarg);
                break;

            case 'k':
                cfg.degree = atoi(optarg);
                break;

            case 'r':
                cfg.ref_levels = atoi(optarg);
                if (cfg.ref_levels < 0)
                    cfg.ref_levels = 1;
                break;

            case 'm':
                if ( strcmp(optarg, "tri") == 0 )
                    mt = yaourt::meshtype::TRIANGULAR;
                else if ( strcmp(optarg, "quad") == 0 )
                    mt = yaourt::meshtype::QUADRANGULAR;
                break;

            case 'p':
                cfg.use_preconditioner = true;
                break;

            case 'S':
                cfg.shatter = true;
                break;

            case 'u':
                cfg.use_upwinding = true;
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
        case yaourt::meshtype::TRIANGULAR:
            run_dg< yaourt::simplicial_mesh<T> >(cfg);
            break;

        case yaourt::meshtype::QUADRANGULAR:
            run_dg< yaourt::quad_mesh<T> >(cfg);
            break;

        default:
            std::cout << "Only triangular and quadrangular meshes for now" << std::endl;
            break;
    }


    return 0;
}
