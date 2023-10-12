// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-734707. All Rights
// reserved. See files LICENSE and NOTICE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project 17-SC-20-SC,
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.
//
//                     __                __
//                    / /   ____  ____  / /_  ____  _____
//                   / /   / __ `/ __ `/ __ \/ __ \/ ___/
//                  / /___/ /_/ / /_/ / / / / /_/ (__  )
//                 /_____/\__,_/\__, /_/ /_/\____/____/
//                             /____/
//
//             High-order Lagrangian Geodynamics Solver
//
// Laghos(LAGrangian High-Order Solver) is a miniapp that solves the
// time-dependent Euler equation of compressible gas dynamics in a moving
// Lagrangian frame using unstructured high-order finite element spatial
// discretization and explicit high-order time-stepping. Laghos is based on the
// numerical algorithm described in the following article:
//
//    V. Dobrev, Tz. Kolev and R. Rieben, "High-order curvilinear finite element
//    methods for Lagrangian geodynamics", SIAM Journal on Scientific
//    Computing, (34) 2012, pp. B606–B641, https://doi.org/10.1137/120864672.
//
//                     __                __               __ 
//                    / /   ____ _____ _/ /_  ____  _____/ /_
//                   / /   / __ `/ __ `/ __ \/ __ \/ ___/ __/
//                  / /___/ /_/ / /_/ / / / / /_/ (__  ) /_  
//                 /_____/\__,_/\__, /_/ /_/\____/____/\__/  
//                             /____/                        
//             Lagrangian High-order Solver for Tectonics
//
// Laghost inherits the main structure of LAGHOS. However, it solves
// the dynamic form of the general momenntum balance equation for continnum,
// aquiring quasi-static solution with dynamic relaxation; reasonably large
// time steps with mass scaling. The target applications include long-term 
// brittle and ductile deformations of rocks coupled with time-evolving 
// thermal state.
//
// -- How to run LAGHOST
// mpirun -np 8 laghost -i ./defaults.cfg

#include <fstream>
#include <sys/time.h>
#include <sys/resource.h>
#include "laghost_solver.hpp"
#include "laghost_rheology.hpp"
#include "laghost_function.hpp"
#include <cmath>
#include "parameters.hpp"
#include "input.hpp"
#include "laghost_tmop.hpp"
#include "laghost_remhos.hpp"

using std::cout;
using std::endl;
using namespace mfem;

// Choice for the problem setup.
static int problem, dim;
static long GetMaxRssMB();
static void display_banner(std::ostream&);
static void Checks(const int ti, const double norm, int &checks);

void TMOPUpdate(BlockVector &S, BlockVector &S_old,
               Array<int> &offset,
               ParGridFunction &x_gf,
               ParGridFunction &v_gf,
               ParGridFunction &e_gf,
               ParGridFunction &s_gf,
               ParGridFunction &x_ini_gf,
               ParGridFunction &p_gf,
               ParGridFunction &n_p_gf,
               ParGridFunction &ini_p_gf,
               ParGridFunction &u_gf,
               ParGridFunction &rho0_gf,
               ParGridFunction &lambda0_gf,
               ParGridFunction &mu0_gf,
               ParGridFunction &mat_gf,
               ParLinearForm &flattening,
               int dim, bool amr);

int main(int argc, char *argv[])
{
   // Initialize MPI.
   mfem::MPI_Session mpi(argc, argv);
   const int myid = mpi.WorldRank();

   // Print the banner.
   if (mpi.Root()) { display_banner(cout); }

   OptionsParser args(argc, argv);
   const char *input_parameter_file = "./defaults.cfg";
   args.AddOption(&input_parameter_file, "-i", "--input", "Input parameter file to use.");

   Param param;
   get_input_parameters(input_parameter_file, param);
   
   Array<int> cxyz; // Leave undefined. It won't be used.
   // double init_dt = 1e-1;
   double blast_energy = 0.0;
   double v_unit = 1.0/86400/365.25;
   // double blast_energy = 1.0e-6;
   double blast_position[] = {0.0, 0.5, 0.0};
   // double blast_energy2 = 0.1;
   // double blast_position2[] = {8.0, 0.5};
   // Mesh bounding box
   Vector bb_min, bb_max;

   // Remeshing performs using the Target-Matrix Optimization Paradigm (TMOP)
   bool mesh_changed = false;
   // int  multi_comp   = 0;
   int  n_dt   = 50;
   //
   double itime{1.0e-100};
   // time_reduction{0.25};
   // Let some options be overwritten by command-line options.
   args.AddOption(&param.sim.dim, "-dim", "--dimension", "Dimension of the problem.");
   args.AddOption(&param.sim.t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&param.sim.max_tsteps, "-ms", "--max-steps",
                  "Maximum number of steps (negative means no restriction).");
   args.AddOption(&param.sim.visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&param.sim.vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.AddOption(&param.sim.visit, "-visit", "--visit", "-no-visit", "--no-visit",
                  "Enable or disable VisIt visualization.");
   args.AddOption(&param.sim.paraview, "-paraview", "--paraview-datafiles", "-no-paraview",
                  "--no-paraview-datafiles",
                  "Save data files for ParaView (paraview.org) visualization.");
   args.AddOption(&param.sim.gfprint, "-print", "--print", "-no-print", "--no-print",
                  "Enable or disable result output (files in mfem format).");
   // args.AddOption(param.sim.basename.c_str(), "-k", "--outputfilename",
   //                "Name of the visit dump files");
   // args.AddOption(param.sim.device.c_str(), "-d", "--device",
   //                "Device configuration string, see Device::Configure().");
   args.AddOption(&param.sim.dev, "-dev", "--dev", "GPU device to use.");
   args.AddOption(&param.sim.check, "-chk", "--checks", "-no-chk", "--no-checks",
                  "Enable 2D checks.");
   args.AddOption(&param.sim.mem_usage, "-mb", "--mem", "-no-mem", "--no-mem",
                  "Enable memory usage.");
   args.AddOption(&param.sim.fom, "-f", "--fom", "-no-fom", "--no-fom",
                  "Enable figure of merit output.");
   args.AddOption(&param.sim.gpu_aware_mpi, "-gam", "--gpu-aware-mpi", "-no-gam",
                  "--no-gpu-aware-mpi", "Enable GPU aware MPI communications.");                      
   // args.AddOption(&param.mesh.mesh_file, "-m", "--mesh", "Mesh file to use.");
   args.AddOption(&param.mesh.rs_levels, "-rs", "--refine-serial",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&param.mesh.rp_levels, "-rp", "--refine-parallel",
                  "Number of times to refine the mesh uniformly in parallel.");
   args.AddOption(&param.mesh.partition_type, "-pt", "--partition",
                  "Customized x/y/z Cartesian MPI partitioning of the serial mesh.\n\t"
                  "Here x,y,z are relative task ratios in each direction.\n\t"
                  "Example: with 48 mpi tasks and -pt 321, one would get a Cartesian\n\t"
                  "partition of the serial mesh by (6,4,2) MPI tasks in (x,y,z).\n\t"
                  "NOTE: the serially refined mesh must have the appropriate number\n\t"
                  "of zones in each direction, e.g., the number of zones in direction x\n\t"
                  "must be divisible by the number of MPI tasks in direction x.\n\t"
                  "Available options: 11, 21, 111, 211, 221, 311, 321, 322, 432.");                  
   args.AddOption(&param.mesh.order_v, "-ok", "--order-kinematic",
                  "Order (degree) of the kinematic finite element space.");
   args.AddOption(&param.mesh.order_e, "-ot", "--order-thermo",
                  "Order (degree) of the thermodynamic finite element space.");
   args.AddOption(&param.mesh.order_q, "-oq", "--order-intrule",
                  "Order  of the integration rule.");
   args.AddOption(&param.solver.ode_solver_type, "-s", "--ode-solver",
                  "ODE solver: 1 - Forward Euler,\n\t"
                  "            2 - RK2 SSP, 3 - RK3 SSP, 4 - RK4, 6 - RK6,\n\t"
                  "            7 - RK2Avg.");
   args.AddOption(&param.solver.cfl, "-cfl", "--cfl", "CFL-condition number.");
   args.AddOption(&param.solver.cg_tol, "-cgt", "--cg-tol",
                  "Relative CG tolerance (velocity linear solve).");
   args.AddOption(&param.solver.ftz_tol, "-ftz", "--ftz-tol",
                  "Absolute flush-to-zero tolerance.");
   args.AddOption(&param.solver.cg_max_iter, "-cgm", "--cg-max-steps",
                  "Maximum number of CG iterations (velocity linear solve).");
   args.AddOption(&param.solver.p_assembly, "-pa", "--partial-assembly", "-fa",
                  "--full-assembly",
                  "Activate 1D tensor-based assembly (partial assembly).");
   args.AddOption(&param.solver.impose_visc, "-iv", "--impose-viscosity", "-niv",
                  "--no-impose-viscosity",
                  "Use active viscosity terms even for smooth problems.");

   // TMOP
   args.AddOption(&param.tmop.tmop, "-TMOP", "--enable-TMOP", "-no-TMOP", "--disable-TMOP",
                  "Target Mesh Optimization Paradigm.");
   args.AddOption(&param.tmop.amr, "-amr", "--enable-amr", "-no-amr", "--disable-amr",
                  "Adaptive mesh refinement.");
   args.AddOption(&param.tmop.remesh_steps, "-rstep", "--remesh_steps",
                  "remeshing frequency.");
   args.AddOption(&param.tmop.jitter, "-ji", "--jitter",
                  "Random perturbation scaling factor.");
   args.AddOption(&param.tmop.metric_id, "-mid", "--metric-id",
                  "Mesh optimization metric:\n\t"
                  "T-metrics\n\t"
                  "1  : |T|^2                          -- 2D no type\n\t"
                  "2  : 0.5|T|^2/tau-1                 -- 2D shape (condition number)\n\t"
                  "7  : |T-T^-t|^2                     -- 2D shape+size\n\t"
                  "9  : tau*|T-T^-t|^2                 -- 2D shape+size\n\t" // not applicable
                  "14 : |T-I|^2                        -- 2D shape+size+orientation\n\t" 
                  "22 : 0.5(|T|^2-2*tau)/(tau-tau_0)   -- 2D untangling\n\t"
                  "50 : 0.5|T^tT|^2/tau^2-1            -- 2D shape\n\t"
                  "55 : (tau-1)^2                      -- 2D size\n\t"
                  "56 : 0.5(sqrt(tau)-1/sqrt(tau))^2   -- 2D size\n\t"
                  "58 : |T^tT|^2/(tau^2)-2*|T|^2/tau+2 -- 2D shape\n\t"
                  "77 : 0.5(tau-1/tau)^2               -- 2D size\n\t"
                  "80 : (1-gamma)mu_2 + gamma mu_77    -- 2D shape+size\n\t"
                  "85 : |T-|T|/sqrt(2)I|^2             -- 2D shape+orientation\n\t"
                  "90 : balanced combo mu_50 & mu_77   -- 2D shape+size\n\t"
                  "94 : balanced combo mu_2 & mu_56    -- 2D shape+size\n\t"
                  "98 : (1/tau)|T-I|^2                 -- 2D shape+size+orientation\n\t"
                  // "211: (tau-1)^2-tau+sqrt(tau^2+eps)  -- 2D untangling\n\t"
                  // "252: 0.5(tau-1)^2/(tau-tau_0)       -- 2D untangling\n\t"
                  "301: (|T||T^-1|)/3-1              -- 3D shape\n\t"
                  "302: (|T|^2|T^-1|^2)/9-1          -- 3D shape\n\t"
                  "303: (|T|^2)/3/tau^(2/3)-1        -- 3D shape\n\t"
                  "304: (|T|^3)/3^{3/2}/tau-1        -- 3D shape\n\t"
                  // "311: (tau-1)^2-tau+sqrt(tau^2+eps)-- 3D untangling\n\t"
                  "313: (|T|^2)(tau-tau0)^(-2/3)/3   -- 3D untangling\n\t"
                  "315: (tau-1)^2                    -- 3D no type\n\t"
                  "316: 0.5(sqrt(tau)-1/sqrt(tau))^2 -- 3D no type\n\t"
                  "321: |T-T^-t|^2                   -- 3D shape+size\n\t"
                  "322: |T-adjT^-t|^2                -- 3D shape+size\n\t"
                  "323: |J|^3-3sqrt(3)ln(det(J))-3sqrt(3)  -- 3D shape+size\n\t"
                  "328: balanced combo mu_301 & mu_316   -- 3D shape+size\n\t"
                  "332: (1-gamma) mu_302 + gamma mu_315  -- 3D shape+size\n\t"
                  "333: (1-gamma) mu_302 + gamma mu_316  -- 3D shape+size\n\t"
                  "334: (1-gamma) mu_303 + gamma mu_316  -- 3D shape+size\n\t"
                  "328: balanced combo mu_302 & mu_318   -- 3D shape+size\n\t"
                  "347: (1-gamma) mu_304 + gamma mu_316  -- 3D shape+size\n\t"
                  // "352: 0.5(tau-1)^2/(tau-tau_0)     -- 3D untangling\n\t"
                  "360: (|T|^3)/3^{3/2}-tau              -- 3D shape\n\t"
                  "A-metrics\n\t"
                  "11 : (1/4*alpha)|A-(adjA)^T(W^TW)/omega|^2 -- 2D shape\n\t"
                  "36 : (1/alpha)|A-W|^2                      -- 2D shape+size+orientation\n\t"
                  "107: (1/2*alpha)|A-|A|/|W|W|^2             -- 2D shape+orientation\n\t"
                  "126: (1-gamma)nu_11 + gamma*nu_14a         -- 2D shape+size\n\t"
                 );
   args.AddOption(&param.tmop.target_id, "-tid", "--target-id",
                  "Target (ideal element) type:\n\t"
                  "1: Ideal shape, unit size\n\t"
                  "2: Ideal shape, equal size\n\t"
                  "3: Ideal shape, initial size\n\t"
                  "4: Given full analytic Jacobian (in physical space)\n\t"
                  "5: Ideal shape, given size (in physical space)");
   args.AddOption(&param.tmop.lim_const, "-lc", "--limit-const", "Limiting constant.");
   args.AddOption(&param.tmop.adapt_lim_const, "-alc", "--adapt-limit-const",
                  "Adaptive limiting coefficient constant.");
   args.AddOption(&param.tmop.quad_type, "-qt", "--quad-type",
                  "Quadrature rule type:\n\t"
                  "1: Gauss-Lobatto\n\t"
                  "2: Gauss-Legendre\n\t"
                  "3: Closed uniform points");
   args.AddOption(&param.tmop.quad_order, "-qo", "--quad_order",
                  "Order of the quadrature rule.");
   args.AddOption(&param.tmop.solver_type, "-st", "--solver-type",
                  " Type of solver: (default) 0: Newton, 1: LBFGS");
   args.AddOption(&param.tmop.solver_iter, "-ni", "--newton-iters",
                  "Maximum number of Newton iterations.");
   args.AddOption(&param.tmop.solver_rtol, "-rtol", "--newton-rel-tolerance",
                  "Relative tolerance for the Newton solver.");
   args.AddOption(&param.tmop.solver_art_type, "-art", "--adaptive-rel-tol",
                  "Type of adaptive relative linear solver tolerance:\n\t"
                  "0: None (default)\n\t"
                  "1: Eisenstat-Walker type 1\n\t"
                  "2: Eisenstat-Walker type 2");
   args.AddOption(&param.tmop.lin_solver, "-ls", "--lin-solver",
                  "Linear solver:\n\t"
                  "0: l1-Jacobi\n\t"
                  "1: CG\n\t"
                  "2: MINRES\n\t"
                  "3: MINRES + Jacobi preconditioner\n\t"
                  "4: MINRES + l1-Jacobi preconditioner");
   args.AddOption(&param.tmop.max_lin_iter, "-li", "--lin-iter",
                  "Maximum number of iterations in the linear solve.");
   args.AddOption(&param.tmop.move_bnd, "-bnd", "--move-boundary", "-fix-bnd",
                  "--fix-boundary",
                  "Enable motion along horizontal and vertical boundaries.");
   args.AddOption(&param.tmop.combomet, "-cmb", "--combo-type",
                  "Combination of metrics options:\n\t"
                  "0: Use single metric\n\t"
                  "1: Shape + space-dependent size given analytically\n\t"
                  "2: Shape + adapted size given discretely; shared target");
   args.AddOption(&param.tmop.bal_expl_combo, "-bec", "--balance-explicit-combo",
                  "-no-bec", "--balance-explicit-combo",
                  "Automatic balancing of explicit combo metrics.");
   args.AddOption(&param.tmop.hradaptivity, "-hr", "--hr-adaptivity", "-no-hr",
                  "--no-hr-adaptivity",
                  "Enable hr-adaptivity.");
   args.AddOption(&param.tmop.h_metric_id, "-hmid", "--h-metric",
                  "Same options as metric_id. Used to determine refinement"
                  " type for each element if h-adaptivity is enabled.");
   args.AddOption(&param.tmop.normalization, "-nor", "--normalization", "-no-nor",
                  "--no-normalization",
                  "Make all terms in the optimization functional unitless.");
   args.AddOption(&param.tmop.fdscheme, "-fd", "--fd_approximation",
                  "-no-fd", "--no-fd-approx",
                  "Enable finite difference based derivative computations.");
   args.AddOption(&param.tmop.exactaction, "-ex", "--exact_action",
                  "-no-ex", "--no-exact-action",
                  "Enable exact action of TMOP_Integrator.");
   args.AddOption(&param.tmop.verbosity_level, "-vl", "--verbosity-level",
                  "Verbosity level for the involved iterative solvers:\n\t"
                  "0: no output\n\t"
                  "1: Newton iterations\n\t"
                  "2: Newton iterations + linear solver summaries\n\t"
                  "3: newton iterations + linear solver iterations");
   args.AddOption(&param.tmop.adapt_eval, "-ae", "--adaptivity-evaluator",
                  "0 - Advection based (DEFAULT), 1 - GSLIB.");
   args.AddOption(&param.tmop.n_hr_iter, "-nhr", "--n_hr_iter",
                  "Number of hr-adaptivity iterations.");
   args.AddOption(&param.tmop.n_h_iter, "-nh", "--n_h_iter",
                  "Number of h-adaptivity iterations per r-adaptivity"
                  "iteration.");
   args.AddOption(&param.tmop.mesh_node_ordering, "-mno", "--mesh_node_ordering",
                  "Ordering of mesh nodes."
                  "0 (default): byNodes, 1: byVDIM");
   args.AddOption(&param.tmop.barrier_type, "-btype", "--barrier-type",
                  "0 - None,"
                  "1 - Shifted Barrier,"
                  "2 - Pseudo Barrier.");
   args.AddOption(&param.tmop.worst_case_type, "-wctype", "--worst-case-type",
                  "0 - None,"
                  "1 - Beta,"
                  "2 - PMean.");

   args.Parse();

   param.tmop.mesh_poly_deg = param.mesh.order_v;

   if (!args.Good())
   {
      if (mpi.Root()) { args.PrintUsage(cout); }
      return 1;
   }
   if (mpi.Root()) { args.PrintOptions(cout); }
   
   if(param.sim.max_tsteps > -1)
   {
      param.sim.t_final = 1.0e38;
   }   
   if(param.sim.year)
   {
      param.sim.t_final = param.sim.t_final * 86400 * 365.25;
      
      if (mpi.Root())
      {
         std::cout << "Use years in output instead of seconds is true" << std::endl;
      }
   }
   else
   {
      // init_dt = 1e-1;
      if (mpi.Root())
      {
         std::cout << "Use seconds in output instead of years is true" << std::endl;
      }
      
   }

   // Configure the device from the command line options
   Device backend;
   backend.Configure(param.sim.device, param.sim.dev);
   if (mpi.Root()) { backend.Print(); }
   backend.SetGPUAwareMPI(param.sim.gpu_aware_mpi);

   // On all processors, use the default builtin 1D/2D/3D mesh or read the
   // serial one given on the command line.
   Mesh *mesh;

   if (param.mesh.mesh_file.compare("default") != 0)
   {
      mesh = new Mesh(param.mesh.mesh_file.c_str(), true, true);
   }
   else
   {
      if (param.sim.dim == 1)
      {
         mesh = new Mesh(Mesh::MakeCartesian1D(2));
         mesh->GetBdrElement(0)->SetAttribute(1);
         mesh->GetBdrElement(1)->SetAttribute(1);
      }
      if (param.sim.dim == 2)
      {
         mesh = new Mesh(Mesh::MakeCartesian2D(2, 2, Element::QUADRILATERAL,
                                               true));
         const int NBE = mesh->GetNBE();
         for (int b = 0; b < NBE; b++)
         {
            Element *bel = mesh->GetBdrElement(b);
            const int attr = (b < NBE/2) ? 2 : 1;
            std::cout<< NBE <<"," << b << "," << attr <<std::endl;
            bel->SetAttribute(attr);
         }
      }
      if (param.sim.dim == 3)
      {
         mesh = new Mesh(Mesh::MakeCartesian3D(2, 2, 2, Element::HEXAHEDRON,
                                               true));
         const int NBE = mesh->GetNBE();
         for (int b = 0; b < NBE; b++)
         {
            Element *bel = mesh->GetBdrElement(b);
            const int attr = (b < NBE/3) ? 3 : (b < 2*NBE/3) ? 1 : 2;
            bel->SetAttribute(attr);
         }
      }
   }
   dim = mesh->Dimension();

   // 1D vs partial assembly sanity check.
   if (param.solver.p_assembly && dim == 1)
   {
      param.solver.p_assembly = false;
      if (mpi.Root())
      {
         cout << "Laghos does not support PA in 1D. Switching to FA." << endl;
      }
   }

   // Refine the mesh in serial to increase the resolution.
   for (int lev = 0; lev < param.mesh.rs_levels; lev++) { mesh->UniformRefinement(); }

   if(param.mesh.local_refinement)
   {
      // Local refiement
      mesh->EnsureNCMesh(true);

      Array<int> refs;
      // for (int i = 0; i < mesh->GetNE(); i++)
      // {
      //    if(mesh->GetAttribute(i) >= 2)
      //    {
      //       refs.Append(i);
      //    }
      // }

      // mesh->GeneralRefinement(refs, 1);
      // refs.DeleteAll();

      for (int i = 0; i < mesh->GetNE(); i++)
      {
         if(mesh->GetAttribute(i) >= 2)
         {
            refs.Append(i);
         }
      }

      mesh->GeneralRefinement(refs, 1);
      refs.DeleteAll();

      for (int i = 0; i < mesh->GetNE(); i++)
      {
         if(mesh->GetAttribute(i) == 3)
         {
            refs.Append(i);
         }
      }

      mesh->GeneralRefinement(refs, 1);
      refs.DeleteAll();

      // for (int i = 0; i < mesh->GetNE(); i++)
      // {
      //    if(mesh->GetAttribute(i) == 3)
      //    {
      //       refs.Append(i);
      //    }
      // }

      // mesh->GeneralRefinement(refs, 1);
      // refs.DeleteAll();
      
      mesh->Finalize(true);
   }

   const int mesh_NE = mesh->GetNE();
   if (mpi.Root())
   {
      cout << "Number of zones in the serial mesh: " << mesh_NE << endl;
   }

   mesh->GetBoundingBox(bb_min, bb_max, max(param.mesh.order_v, 1));

   // Parallel partitioning of the mesh.
   ParMesh *pmesh = nullptr;
   const int num_tasks = mpi.WorldSize(); int unit = 1;
   int *nxyz = new int[dim];
   switch (param.mesh.partition_type)
   {
      case 0:
         for (int d = 0; d < dim; d++) { nxyz[d] = unit; }
         break;
      case 11:
      case 111:
         unit = static_cast<int>(floor(pow(num_tasks, 1.0 / dim) + 1e-2));
         for (int d = 0; d < dim; d++) { nxyz[d] = unit; }
         break;
      case 21: // 2D
         unit = static_cast<int>(floor(pow(num_tasks / 2, 1.0 / 2) + 1e-2));
         nxyz[0] = 2 * unit; nxyz[1] = unit;
         break;
      case 31: // 2D
         unit = static_cast<int>(floor(pow(num_tasks / 3, 1.0 / 2) + 1e-2));
         nxyz[0] = 3 * unit; nxyz[1] = unit;
         break;
      case 32: // 2D
         unit = static_cast<int>(floor(pow(2 * num_tasks / 3, 1.0 / 2) + 1e-2));
         nxyz[0] = 3 * unit / 2; nxyz[1] = unit;
         break;
      case 49: // 2D
         unit = static_cast<int>(floor(pow(9 * num_tasks / 4, 1.0 / 2) + 1e-2));
         nxyz[0] = 4 * unit / 9; nxyz[1] = unit;
         break;
      case 51: // 2D
         unit = static_cast<int>(floor(pow(num_tasks / 5, 1.0 / 2) + 1e-2));
         nxyz[0] = 5 * unit; nxyz[1] = unit;
         break;
      case 211: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 2, 1.0 / 3) + 1e-2));
         nxyz[0] = 2 * unit; nxyz[1] = unit; nxyz[2] = unit;
         break;
      case 221: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 4, 1.0 / 3) + 1e-2));
         nxyz[0] = 2 * unit; nxyz[1] = 2 * unit; nxyz[2] = unit;
         break;
      case 311: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 3, 1.0 / 3) + 1e-2));
         nxyz[0] = 3 * unit; nxyz[1] = unit; nxyz[2] = unit;
         break;
      case 321: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 6, 1.0 / 3) + 1e-2));
         nxyz[0] = 3 * unit; nxyz[1] = 2 * unit; nxyz[2] = unit;
         break;
      case 322: // 3D.
         unit = static_cast<int>(floor(pow(2 * num_tasks / 3, 1.0 / 3) + 1e-2));
         nxyz[0] = 3 * unit / 2; nxyz[1] = unit; nxyz[2] = unit;
         break;
      case 432: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 3, 1.0 / 3) + 1e-2));
         nxyz[0] = 2 * unit; nxyz[1] = 3 * unit / 2; nxyz[2] = unit;
         break;
      case 511: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 5, 1.0 / 3) + 1e-2));
         nxyz[0] = 5 * unit; nxyz[1] = unit; nxyz[2] = unit;
         break;
      case 521: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 10, 1.0 / 3) + 1e-2));
         nxyz[0] = 5 * unit; nxyz[1] = 2 * unit; nxyz[2] = unit;
         break;
      case 522: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 20, 1.0 / 3) + 1e-2));
         nxyz[0] = 5 * unit; nxyz[1] = 2 * unit; nxyz[2] = 2 * unit;
         break;
      case 911: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 9, 1.0 / 3) + 1e-2));
         nxyz[0] = 9 * unit; nxyz[1] = unit; nxyz[2] = unit;
         break;
      case 921: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 18, 1.0 / 3) + 1e-2));
         nxyz[0] = 9 * unit; nxyz[1] = 2 * unit; nxyz[2] = unit;
         break;
      case 922: // 3D.
         unit = static_cast<int>(floor(pow(num_tasks / 36, 1.0 / 3) + 1e-2));
         nxyz[0] = 9 * unit; nxyz[1] = 2 * unit; nxyz[2] = 2 * unit;
         break;
      default:
         if (myid == 0)
         {
            cout << "Unknown partition type: " << param.mesh.partition_type << '\n';
         }
         delete mesh;
         MPI_Finalize();
         return 3;
   }
   int product = 1;
   for (int d = 0; d < dim; d++) { product *= nxyz[d]; }
   const bool cartesian_partitioning = (cxyz.Size()>0)?true:false;
   if (product == num_tasks || cartesian_partitioning)
   {
      if (cartesian_partitioning)
      {
         int cproduct = 1;
         for (int d = 0; d < dim; d++) { cproduct *= cxyz[d]; }
         MFEM_VERIFY(!cartesian_partitioning || cxyz.Size() == dim,
                     "Expected " << mesh->SpaceDimension() << " integers with the "
                     "option --cartesian-partitioning.");
         MFEM_VERIFY(!cartesian_partitioning || num_tasks == cproduct,
                     "Expected cartesian partitioning product to match number of ranks.");
      }
      int *partitioning = cartesian_partitioning ?
                          mesh->CartesianPartitioning(cxyz):
                          mesh->CartesianPartitioning(nxyz);
      pmesh = new ParMesh(MPI_COMM_WORLD, *mesh, partitioning);
      delete [] partitioning;
   }
   else
   {
      if (myid == 0)
      {
         cout << "Non-Cartesian partitioning through METIS will be used.\n";
#ifndef MFEM_USE_METIS
         cout << "MFEM was built without METIS. "
              << "Adjust the number of tasks to use a Cartesian split." << endl;
#endif
      }
#ifndef MFEM_USE_METIS
      return 1;
#endif
      pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   }
   delete [] nxyz;
   delete mesh;

   // Refine the mesh further in parallel to increase the resolution.
   for (int lev = 0; lev < param.mesh.rp_levels; lev++) { pmesh->UniformRefinement(); }

   // pmesh->Rebalance();

   int NE = pmesh->GetNE(), ne_min, ne_max;
   MPI_Reduce(&NE, &ne_min, 1, MPI_INT, MPI_MIN, 0, pmesh->GetComm());
   MPI_Reduce(&NE, &ne_max, 1, MPI_INT, MPI_MAX, 0, pmesh->GetComm());
   if (myid == 0)
   { cout << "Zones min/max: " << ne_min << " " << ne_max << endl; }

   // Define the parallel finite element spaces. We use:
   // - H1 (Gauss-Lobatto, continuous) for position and velocity.
   // - L2 (Bernstein, discontinuous) for specific internal energy.
   L2_FECollection L2FEC(param.mesh.order_e, dim, BasisType::Positive);
   H1_FECollection H1FEC(param.mesh.order_v, dim);
   ParFiniteElementSpace L2FESpace(pmesh, &L2FEC);
   ParFiniteElementSpace H1FESpace(pmesh, &H1FEC, pmesh->Dimension());
   ParFiniteElementSpace L2FESpace_stress(pmesh, &L2FEC, 3*(dim-1)); // three varibles for 2D, six varibles for 3D

   // Boundary conditions: all tests use v.n = 0 on the boundary, and we assume
   // that the boundaries are straight.
   // Remove square brackets and spaces
   param.bc.bc_ids.erase(std::remove(param.bc.bc_ids.begin(), param.bc.bc_ids.end(), '['), param.bc.bc_ids.end());
   param.bc.bc_ids.erase(std::remove(param.bc.bc_ids.begin(), param.bc.bc_ids.end(), ']'), param.bc.bc_ids.end());
   param.bc.bc_ids.erase(std::remove(param.bc.bc_ids.begin(), param.bc.bc_ids.end(), ' '), param.bc.bc_ids.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss(param.bc.bc_ids);
   std::vector<int> bc_id;
    
   // Temporary variable to store each token
   std::string token;

   // std::cout <<"check point1"<<std::endl;
   // Tokenize the string and convert tokens to integers
   while (getline(ss, token, ',')) 
   {bc_id.push_back(std::stoi(token)); // Convert string to int and add to vector
   }

   // std::cout <<"check point2"<<std::endl;

   if(pmesh->bdr_attributes.Max() != bc_id.size())
   {
      if (myid == 0){cout << "The number of boundaries are not consistent with the given mesh. \nBC indicator from mesh is " << pmesh->bdr_attributes.Max() << " but input is " << bc_id.size() << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }

   // Boundary velocity of x component
   param.bc.bc_vxs.erase(std::remove(param.bc.bc_vxs.begin(), param.bc.bc_vxs.end(), '['), param.bc.bc_vxs.end());
   param.bc.bc_vxs.erase(std::remove(param.bc.bc_vxs.begin(), param.bc.bc_vxs.end(), ']'), param.bc.bc_vxs.end());
   param.bc.bc_vxs.erase(std::remove(param.bc.bc_vxs.begin(), param.bc.bc_vxs.end(), ' '), param.bc.bc_vxs.end());

   // Create a stringstream to tokenize the string
   std::stringstream vxs(param.bc.bc_vxs);
   std::vector<double> bc_vx;
    
   // Tokenize the string and convert tokens to integers
   while (getline(vxs, token, ',')) 
   {bc_vx.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // Boundary velocity of y component
   param.bc.bc_vys.erase(std::remove(param.bc.bc_vys.begin(), param.bc.bc_vys.end(), '['), param.bc.bc_vys.end());
   param.bc.bc_vys.erase(std::remove(param.bc.bc_vys.begin(), param.bc.bc_vys.end(), ']'), param.bc.bc_vys.end());
   param.bc.bc_vys.erase(std::remove(param.bc.bc_vys.begin(), param.bc.bc_vys.end(), ' '), param.bc.bc_vys.end());

   // Create a stringstream to tokenize the string
   std::stringstream vys(param.bc.bc_vys);
   std::vector<double> bc_vy;

   // Tokenize the string and convert tokens to integers
   while (getline(vys, token, ',')) 
   {bc_vy.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // Boundary velocity of z component
   param.bc.bc_vzs.erase(std::remove(param.bc.bc_vzs.begin(), param.bc.bc_vzs.end(), '['), param.bc.bc_vzs.end());
   param.bc.bc_vzs.erase(std::remove(param.bc.bc_vzs.begin(), param.bc.bc_vzs.end(), ']'), param.bc.bc_vzs.end());
   param.bc.bc_vzs.erase(std::remove(param.bc.bc_vzs.begin(), param.bc.bc_vzs.end(), ' '), param.bc.bc_vzs.end());

   // Create a stringstream to tokenize the string
   std::stringstream vzs(param.bc.bc_vzs);
   std::vector<double> bc_vz;

   // Tokenize the string and convert tokens to integers
   while (getline(vzs, token, ',')) 
   {bc_vz.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   if(param.bc.bc_unit =="cm/yr")
   {v_unit = v_unit/100.0;}
   else if(param.bc.bc_unit =="mm/yr")
   {v_unit = v_unit/1000.0;}

   // Dirichlet type boundary condition (i.e., fixing velocity component at boundaries)
   Array<int> ess_tdofs, ess_vdofs;
   {
      Array<int> ess_bdr(pmesh->bdr_attributes.Max()), dofs_marker, dofs_list;
      for (int i = 0; i < bc_id.size(); ++i) 
      {
        ess_bdr = 0;
        if(bc_id[i] > 0)
        {
            if(dim == 2)
            {
               switch (bc_id[i])
               {
                  // case 1 : x compoent is constained
                  // case 2 : y compoent is constained
                  // case 3 : all compoents are constained
                  case 1: ess_bdr[i] = 1; H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,0); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); break;
                  case 2: ess_bdr[i] = 1; H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,1); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); break;
                  case 3: ess_bdr[i] = 1; H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); break;
                  default:
                     if (myid == 0)
                     {
                        cout << "Unknown boundary type: " << bc_id[i] << '\n';
                     }
                     delete pmesh;
                     MPI_Finalize();
                     return 3;
               }
            }
            else
            {
               switch (bc_id[i])
               {
                  // case 1 : x compoent is constained
                  // case 2 : y compoent is constained
                  // case 3 : z compoent is constained
                  // case 4 : all compoents are constained
                  // case 5 : x and y compoents are constained
                  // case 6 : x and z compoents are constained
                  // case 7 : y and z compoents are constained
                  case 1: ess_bdr[i] = 1; H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,0); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); break;
                  case 2: ess_bdr[i] = 1; H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,1); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); break;
                  case 3: ess_bdr[i] = 1; H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,2); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,2); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); break;
                  case 4: ess_bdr[i] = 1; H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); break;
                  case 5: 
                     ess_bdr[i] = 1; 
                     H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,0); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); 
                     H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,1); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); 
                     break;
                  case 6: 
                     ess_bdr[i] = 1; 
                     H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,1); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); 
                     H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,2); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,2); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); 
                     break;
                  case 7: 
                     ess_bdr[i] = 1; 
                     H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,2); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,2); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); 
                     H1FESpace.GetEssentialTrueDofs(ess_bdr, dofs_list,3); ess_tdofs.Append(dofs_list); H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,3); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list); ess_vdofs.Append(dofs_list); 

                  default:
                     if (myid == 0)
                     {
                        cout << "Unknown boundary type: " << bc_id[i] << '\n';
                     }
                     delete pmesh;
                     MPI_Finalize();
                     return 3;
               }
            }
        }
      }
   }

   Vector bc_id_pa(pmesh->bdr_attributes.Max());
   for (int i = 0; i < bc_id.size(); ++i){bc_id_pa[i]=bc_id[i];}

   // Define the explicit ODE solver used for time integration.
   ODESolver *ode_solver = NULL;
   switch (param.solver.ode_solver_type)
   {
      case 1: ode_solver = new ForwardEulerSolver; break;
      case 2: ode_solver = new RK2Solver(0.5); break;
      case 3: ode_solver = new RK3SSPSolver; break;
      case 4: ode_solver = new RK4Solver; break;
      case 6: ode_solver = new RK6Solver; break;
      case 7: ode_solver = new RK2AvgSolver; break;
      default:
         if (myid == 0)
         {
            cout << "Unknown ODE solver type: " << param.solver.ode_solver_type << '\n';
         }
         delete pmesh;
         MPI_Finalize();
         return 3;
   }

   const HYPRE_Int glob_size_l2 = L2FESpace.GlobalTrueVSize();
   const HYPRE_Int glob_size_h1 = H1FESpace.GlobalTrueVSize();
   if (mpi.Root())
   {
      cout << "Number of kinematic (position, velocity) dofs: "
           << glob_size_h1 << endl;
      cout << "Number of specific internal energy dofs: "
           << glob_size_l2 << endl;
   }

   // The monolithic BlockVector stores unknown fields as:
   // - 0 -> position
   // - 1 -> velocity
   // - 2 -> specific internal energy
   // - 3 -> stress
   // - 4 -> plastic strain
   const int Vsize_l2 = L2FESpace.GetVSize();
   const int Vsize_h1 = H1FESpace.GetVSize();
   // Array<int> offset(5);
   Array<int> offset(6); // when you change this number, you should chnage block offset in solver.cpp too
   offset[0] = 0;
   offset[1] = offset[0] + Vsize_h1;
   offset[2] = offset[1] + Vsize_h1;
   offset[3] = offset[2] + Vsize_l2;
   offset[4] = offset[3] + Vsize_l2*3*(dim-1);
   offset[5] = offset[4] + Vsize_h1;
   // offset[5] = offset[4] + Vsize_l2;
   BlockVector S(offset, Device::GetMemoryType());

   // Define GridFunction objects for the position, velocity and specific
   // internal energy. There is no function for the density, as we can always
   // compute the density values given the current mesh position, using the
   // property of pointwise mass conservation.
   ParGridFunction x_gf, v_gf, e_gf, s_gf;
   ParGridFunction x_ini_gf;
   x_gf.MakeRef(&H1FESpace, S, offset[0]);
   v_gf.MakeRef(&H1FESpace, S, offset[1]);
   e_gf.MakeRef(&L2FESpace, S, offset[2]);
   s_gf.MakeRef(&L2FESpace_stress, S, offset[3]);
   x_ini_gf.MakeRef(&H1FESpace, S, offset[4]);

   // Initialize x_gf using the starting mesh coordinates.
   pmesh->SetNodalGridFunction(&x_gf);
   // Sync the data location of x_gf with its base, S
   x_gf.SyncAliasMemory(S);


   // // Create a "sub mesh" from the boundary elements with attribute 3 (top boundary) for Surface process
   // Array<int> bdr_attrs(1);
   // bdr_attrs[0] = 4;
   // ParSubMesh submesh(ParSubMesh::CreateFromBoundary(*pmesh, bdr_attrs));
   // ParFiniteElementSpace sub_fespace0(&submesh, &H1FEC, pmesh->Dimension()); // nodes of submesh
   // ParFiniteElementSpace sub_fespace1(&submesh, &H1FEC); // topography

   // // Solve a Poisson problem on the boundary. This just follows ex0p.
   // Array<int> boundary_dofs;
   // sub_fespace1.GetBoundaryTrueDofs(boundary_dofs);
   // ParGridFunction x_top(&sub_fespace0);
   // ParGridFunction topo(&sub_fespace1);
   // topo=param.control.thickness;
   // submesh.SetNodalGridFunction(&x_top);
   
   // for (int i = 0; i < x.Size(); i++)
   // {
   //   std::cout << i << "/"<< x.Size() << "," << x[i] << std::endl;
   // }

   // for (int i = 0; i < boundary_dofs.Size(); i++)
   // {
   //   std::cout << i << "/"<< boundary_dofs.Size() << "," << x[boundary_dofs[i]] << std::endl;
   // }

   // ParaViewDataCollection *pdd = NULL;
   // if (param.sim.paraview)
   // {
   //    pdd = new ParaViewDataCollection("test", &submesh);
   //    pdd->SetPrefixPath("ParaView");
   //    pdd->RegisterField("test",  &x);
   //    pdd->SetDataFormat(VTKFormat::BINARY);
   //    pdd->SetHighOrderOutput(true);
   //    pdd->SetCycle(0);
   //    pdd->SetTime(0.0);
   //    pdd->Save();
   // }


   // ParSubMesh::Transfer(x, x_gf); // update adjusted nodes on top boundary 

   // Array<int> boundary_dofs;
   // sub_fespace.GetBoundaryTrueDofs(boundary_dofs);
   // ParGridFunction x_top(&sub_fespace);
   // // std::cout<<x_top.Size() <<std::endl;
   // x_top = param.control.thickness;
   // ConstantCoefficient diffusivity(1e-6);
   // ParLinearForm b_top(&sub_fespace);
   // b_top.AddDomainIntegrator(new DomainLFIntegrator(diffusivity));
   // b_top.Assemble();
   // ParBilinearForm a_top(&sub_fespace);
   // a_top.AddDomainIntegrator(new DiffusionIntegrator);
   // a_top.Assemble();

   // // Surface diffusion
   // HypreParMatrix A_top;
   // Vector B_top, X_top;
   // // b_top *= dt;
   // a_top.FormLinearSystem(boundary_dofs, x_top, b_top, A_top, X_top, B_top);
   // HypreBoomerAMG M_top(A_top);
   // CGSolver cg(MPI_COMM_WORLD);
   // cg.SetRelTol(1e-12);
   // cg.SetMaxIter(2000);
   // cg.SetPrintLevel(1);
   // cg.SetPreconditioner(M_top);
   // cg.SetOperator(A_top);
   // cg.Mult(B_top, X_top);
   // a_top.RecoverFEMSolution(X_top, b_top, x_top);
   // ParSubMesh::Transfer(x_top, x_gf); // update adjusted nodes on top boundary 


   // xyz coordinates in L2 space
   // ParGridFunction x_gf_l2(&L2FESpace);
   // ParGridFunction y_gf_l2(&L2FESpace);
   // ParGridFunction z_gf_l2(&L2FESpace);
   // x_gf_l2 = 0.0; y_gf_l2 = 0.0; z_gf_l2 = 0.0;
   // FunctionCoefficient x_coeff(x_l2);
   // FunctionCoefficient y_coeff(y_l2);
   // x_gf_l2.ProjectCoefficient(x_coeff);
   // y_gf_l2.ProjectCoefficient(y_coeff);

   // if(dim == 3)
   // {
   //    FunctionCoefficient z_coeff(z_l2);
   //    z_gf_l2.ProjectCoefficient(z_coeff);
   // }

   // xyz coordinates in L2 space
   ParFiniteElementSpace L2FESpace_xyz(pmesh, &L2FEC, dim); //
   ParGridFunction xyz_gf_l2(&L2FESpace_xyz);
   VectorFunctionCoefficient xyz_coeff(pmesh->Dimension(), xyz0);
   xyz_gf_l2.ProjectCoefficient(xyz_coeff);

   // Initialize the velocity.
   v_gf = 0.0;
   // PlasticCoefficient p_coeff(dim, xyz_gf_l2, weak_location, param.mat.weak_rad, param.mat.ini_pls);
   VectorFunctionCoefficient v_coeff(pmesh->Dimension(), v0);
   v_gf.ProjectCoefficient(v_coeff);

   for (int i = 0; i < bc_id.size(); ++i) 
   {
      Array<int> ess_bdr(pmesh->bdr_attributes.Max()), dofs_marker, dofs_list1, dofs_list2, dofs_list3;
      if(bc_id[i] > 0)
      {
         ess_bdr = 0;
         if(dim == 2)
         {
            switch (bc_id[i])
            {
               // case 1 : x compoent is constained
               // case 2 : y compoent is constained
               // case 3 : all compoents are constained
               case 1: ess_bdr[i] = 1; H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list1); for (int j = 0; j < dofs_list1.Size(); j++){v_gf(dofs_list1[j]) = v_unit*bc_vx[i];} break;
               case 2: ess_bdr[i] = 1; H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list2); for (int j = 0; j < dofs_list2.Size(); j++){v_gf(dofs_list2[j]) = v_unit*bc_vy[i];} break;
               case 3: ess_bdr[i] = 1; 
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list1); for (int j = 0; j < dofs_list1.Size(); j++){v_gf(dofs_list1[j]) = v_unit*bc_vx[i];}
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list2); for (int j = 0; j < dofs_list2.Size(); j++){v_gf(dofs_list2[j]) = v_unit*bc_vy[i];} break;
               
               default:
                  if (myid == 0)
                  {
                        cout << "Unknown boundary type: " << bc_id[i] << '\n';
                  }
                  delete pmesh;
                  MPI_Finalize();
                  return 3;
            }
         }
         else
         {
            switch (bc_id[i])
            {
               // case 1 : x compoent is constained
               // case 2 : y compoent is constained
               // case 3 : z compoent is constained
               // case 4 : all compoents are constained
               // case 5 : x and y compoents are constained
               // case 6 : x and z compoents are constained
               // case 7 : y and z compoents are constained
               case 1: ess_bdr[i] = 1; H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list1); for (int j = 0; j < dofs_list1.Size(); j++){v_gf(dofs_list1[j]) = v_unit*bc_vx[i];} break;
               case 2: ess_bdr[i] = 1; H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list2); for (int j = 0; j < dofs_list2.Size(); j++){v_gf(dofs_list2[j]) = v_unit*bc_vy[i];} break;
               case 3: ess_bdr[i] = 1; H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,2); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list3); for (int j = 0; j < dofs_list3.Size(); j++){v_gf(dofs_list3[j]) = v_unit*bc_vz[i];} break;
               case 4: ess_bdr[i] = 1; 
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list1); for (int j = 0; j < dofs_list1.Size(); j++){v_gf(dofs_list1[j]) = v_unit*bc_vx[i];}
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list2); for (int j = 0; j < dofs_list2.Size(); j++){v_gf(dofs_list2[j]) = v_unit*bc_vy[i];}
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,2); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list3); for (int j = 0; j < dofs_list3.Size(); j++){v_gf(dofs_list3[j]) = v_unit*bc_vz[i];} break;
               case 5: ess_bdr[i] = 1; 
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list1); for (int j = 0; j < dofs_list1.Size(); j++){v_gf(dofs_list1[j]) = v_unit*bc_vx[i];}
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list2); for (int j = 0; j < dofs_list2.Size(); j++){v_gf(dofs_list2[j]) = v_unit*bc_vy[i];} break;
               case 6: ess_bdr[i] = 1;
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,0); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list1); for (int j = 0; j < dofs_list1.Size(); j++){v_gf(dofs_list1[j]) = v_unit*bc_vx[i];}
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,2); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list3); for (int j = 0; j < dofs_list3.Size(); j++){v_gf(dofs_list3[j]) = v_unit*bc_vz[i];} break;
               case 7: ess_bdr[i] = 1;
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,1); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list2); for (int j = 0; j < dofs_list2.Size(); j++){v_gf(dofs_list2[j]) = v_unit*bc_vy[i];}
                       H1FESpace.GetEssentialVDofs(ess_bdr, dofs_marker,2); FiniteElementSpace::MarkerToList(dofs_marker, dofs_list3); for (int j = 0; j < dofs_list3.Size(); j++){v_gf(dofs_list3[j]) = v_unit*bc_vz[i];} break;

               default:
                  if (myid == 0)
                  {
                     cout << "Unknown boundary type: " << bc_id[i] << '\n';
                  }
                  delete pmesh;
                  MPI_Finalize();
                  return 3;
            }
         }
      }
   }

   // for (int i = 0; i < ess_vdofs.Size(); i++)
   // {
   //    v_gf(ess_vdofs[i]) = 0.0;
   // }

   // For the Sedov test, we use a delta function at the origin.
   // Vector dir(pmesh->Dimension());
   // L2_FECollection h1_fec(order_v, pmesh->Dimension());
   // ParFiniteElementSpace h1_fes(pmesh, &h1_fec);
   // ParGridFunction h1_v(&h1_fes);
   // dir=0.0;
   // dir(1)=1.0;
   // // VectorDeltaCoefficient v2_coeff(pmesh->Dimension());
   // // v2_coeff.SetScale(blast_energy2);
   // // v2_coeff.SetDeltaCenter(cent);
   // // v2_coeff.SetDirection(dir);
   // VectorDeltaCoefficient v2_coeff(dir, blast_position2[0], blast_position2[1], blast_energy2);
  
   // h1_v.ProjectCoefficient(v2_coeff);
   // // v_gf.ProjectGridFunction(h1_v);

   // Sync the data location of v_gf with its base, S
   v_gf.SyncAliasMemory(S);

   // String (Material) extraction
   param.mat.rho.erase(std::remove(param.mat.rho.begin(), param.mat.rho.end(), '['), param.mat.rho.end());
   param.mat.rho.erase(std::remove(param.mat.rho.begin(), param.mat.rho.end(), ']'), param.mat.rho.end());
   param.mat.rho.erase(std::remove(param.mat.rho.begin(), param.mat.rho.end(), ' '), param.mat.rho.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss2(param.mat.rho);
   std::vector<double> rho_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss2, token, ',')) 
   {rho_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   param.mat.lambda.erase(std::remove(param.mat.lambda.begin(), param.mat.lambda.end(), '['), param.mat.lambda.end());
   param.mat.lambda.erase(std::remove(param.mat.lambda.begin(), param.mat.lambda.end(), ']'), param.mat.lambda.end());
   param.mat.lambda.erase(std::remove(param.mat.lambda.begin(), param.mat.lambda.end(), ' '), param.mat.lambda.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss3(param.mat.lambda);
   std::vector<double> lambda_vec;

   // Tokenize the string and convert tokens to integers
   while (getline(ss3, token, ',')) 
   {
      lambda_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.mu.erase(std::remove(param.mat.mu.begin(), param.mat.mu.end(), '['), param.mat.mu.end());
   param.mat.mu.erase(std::remove(param.mat.mu.begin(), param.mat.mu.end(), ']'), param.mat.mu.end());
   param.mat.mu.erase(std::remove(param.mat.mu.begin(), param.mat.mu.end(), ' '), param.mat.mu.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss4(param.mat.mu);
   std::vector<double> mu_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss4, token, ',')) 
   {mu_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.tension_cutoff.erase(std::remove(param.mat.tension_cutoff.begin(), param.mat.tension_cutoff.end(), '['), param.mat.tension_cutoff.end());
   param.mat.tension_cutoff.erase(std::remove(param.mat.tension_cutoff.begin(), param.mat.tension_cutoff.end(), ']'), param.mat.tension_cutoff.end());
   param.mat.tension_cutoff.erase(std::remove(param.mat.tension_cutoff.begin(), param.mat.tension_cutoff.end(), ' '), param.mat.tension_cutoff.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss5(param.mat.tension_cutoff);
   std::vector<double> tension_cutoff_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss5, token, ',')) 
   {tension_cutoff_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }
   
   // String (Material) extraction
   param.mat.cohesion0.erase(std::remove(param.mat.cohesion0.begin(), param.mat.cohesion0.end(), '['), param.mat.cohesion0.end());
   param.mat.cohesion0.erase(std::remove(param.mat.cohesion0.begin(), param.mat.cohesion0.end(), ']'), param.mat.cohesion0.end());
   param.mat.cohesion0.erase(std::remove(param.mat.cohesion0.begin(), param.mat.cohesion0.end(), ' '), param.mat.cohesion0.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss6(param.mat.cohesion0);
   std::vector<double> cohesion0_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss6, token, ',')) 
   {cohesion0_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.cohesion1.erase(std::remove(param.mat.cohesion1.begin(), param.mat.cohesion1.end(), '['), param.mat.cohesion1.end());
   param.mat.cohesion1.erase(std::remove(param.mat.cohesion1.begin(), param.mat.cohesion1.end(), ']'), param.mat.cohesion1.end());
   param.mat.cohesion1.erase(std::remove(param.mat.cohesion1.begin(), param.mat.cohesion1.end(), ' '), param.mat.cohesion1.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss7(param.mat.cohesion1);
   std::vector<double> cohesion1_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss7, token, ',')) 
   {cohesion1_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.friction_angle.erase(std::remove(param.mat.friction_angle.begin(), param.mat.friction_angle.end(), '['), param.mat.friction_angle.end());
   param.mat.friction_angle.erase(std::remove(param.mat.friction_angle.begin(), param.mat.friction_angle.end(), ']'), param.mat.friction_angle.end());
   param.mat.friction_angle.erase(std::remove(param.mat.friction_angle.begin(), param.mat.friction_angle.end(), ' '), param.mat.friction_angle.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss8(param.mat.friction_angle);
   std::vector<double> friction_angle_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss8, token, ',')) 
   {friction_angle_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.dilation_angle.erase(std::remove(param.mat.dilation_angle.begin(), param.mat.dilation_angle.end(), '['), param.mat.dilation_angle.end());
   param.mat.dilation_angle.erase(std::remove(param.mat.dilation_angle.begin(), param.mat.dilation_angle.end(), ']'), param.mat.dilation_angle.end());
   param.mat.dilation_angle.erase(std::remove(param.mat.dilation_angle.begin(), param.mat.dilation_angle.end(), ' '), param.mat.dilation_angle.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss9(param.mat.dilation_angle);
   std::vector<double> dilation_angle_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss9, token, ',')) 
   {dilation_angle_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.pls0.erase(std::remove(param.mat.pls0.begin(), param.mat.pls0.end(), '['), param.mat.pls0.end());
   param.mat.pls0.erase(std::remove(param.mat.pls0.begin(), param.mat.pls0.end(), ']'), param.mat.pls0.end());
   param.mat.pls0.erase(std::remove(param.mat.pls0.begin(), param.mat.pls0.end(), ' '), param.mat.pls0.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss10(param.mat.pls0);
   std::vector<double> pls0_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss10, token, ',')) 
   {pls0_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.pls1.erase(std::remove(param.mat.pls1.begin(), param.mat.pls1.end(), '['), param.mat.pls1.end());
   param.mat.pls1.erase(std::remove(param.mat.pls1.begin(), param.mat.pls1.end(), ']'), param.mat.pls1.end());
   param.mat.pls1.erase(std::remove(param.mat.pls1.begin(), param.mat.pls1.end(), ' '), param.mat.pls1.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss11(param.mat.pls1);
   std::vector<double> pls1_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss11, token, ',')) 
   {pls1_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // String (Material) extraction
   param.mat.plastic_viscosity.erase(std::remove(param.mat.plastic_viscosity.begin(), param.mat.plastic_viscosity.end(), '['), param.mat.plastic_viscosity.end());
   param.mat.plastic_viscosity.erase(std::remove(param.mat.plastic_viscosity.begin(), param.mat.plastic_viscosity.end(), ']'), param.mat.plastic_viscosity.end());
   param.mat.plastic_viscosity.erase(std::remove(param.mat.plastic_viscosity.begin(), param.mat.plastic_viscosity.end(), ' '), param.mat.plastic_viscosity.end());

   // Create a stringstream to tokenize the string
   std::stringstream ss12(param.mat.plastic_viscosity);
   std::vector<double> plastic_viscosity_vec;
    
   // Tokenize the string and convert tokens to integers
   while (getline(ss12, token, ',')) 
   {plastic_viscosity_vec.push_back(std::stod(token)); // Convert string to int and add to vector
   }

   // Initialize density and specific internal energy values. We interpolate in
   // a non-positive basis to get the correct values at the dofs. Then we do an
   // L2 projection to the positive basis in which we actually compute. The goal
   // is to get a high-order representation of the initial condition. Note that
   // this density is a temporary function and it will not be updated during the
   // time evolution.
   Vector z_rho(pmesh->attributes.Max());
   Vector s_rho(pmesh->attributes.Max());

   // std::cout << "rho, " << rho_vec[0] << std::endl;
   // std::cout << "lambda, " << lambda_vec[0] << std::endl;
   // std::cout << "mu, " << mu_vec[0] << std::endl;
   // std::cout << "tension_cutoff, " << tension_cutoff_vec[0] << std::endl;
   // std::cout << "cohesion0, " << cohesion0_vec[0] << std::endl;
   // std::cout << "cohesion1, " << cohesion1_vec[0] << std::endl;
   // std::cout << "friction_angle, " << friction_angle_vec[0] << std::endl;
   // std::cout << "dilation_angle, " << dilation_angle_vec[0] << std::endl;
   // std::cout << "pls0, " << pls0_vec[0] << std::endl;
   // std::cout << "pls1, " << pls1_vec[0] << std::endl;

   if(rho_vec.size() == 1) {z_rho =rho_vec[0]; s_rho =rho_vec[0] * param.control.mscale;}
   else if(rho_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of rho are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {z_rho[i] = rho_vec[i]; s_rho[i] = rho_vec[i] * param.control.mscale;}}
   
   // z_rho = 2700.0;
   // s_rho = 2700.0 * param.control.mscale;

   ParGridFunction rho0_gf(&L2FESpace);
   PWConstCoefficient rho0_coeff(z_rho);
   PWConstCoefficient scale_rho0_coeff(s_rho);
   L2_FECollection l2_fec(param.mesh.order_e, pmesh->Dimension());
   ParFiniteElementSpace l2_fes(pmesh, &l2_fec);
   ParGridFunction l2_rho0_gf(&l2_fes), l2_e(&l2_fes);
   l2_rho0_gf.ProjectCoefficient(rho0_coeff);
   rho0_gf.ProjectGridFunction(l2_rho0_gf);

   /*
   ParGridFunction rho0_gf(&L2FESpace);
   FunctionCoefficient rho0_coeff(rho0);
   // FunctionCoefficient super_rho0_coeff(rho0);
   L2_FECollection l2_fec(order_e, pmesh->Dimension());
   ParFiniteElementSpace l2_fes(pmesh, &l2_fec);
   ParGridFunction l2_rho0_gf(&l2_fes), l2_e(&l2_fes);
   l2_rho0_gf.ProjectCoefficient(rho0_coeff);
   rho0_gf.ProjectGridFunction(l2_rho0_gf);
   */
   
   if (param.sim.problem == 1)
   {
      // For the Sedov test, we use a delta function at the origin.
      DeltaCoefficient e_coeff(blast_position[0], blast_position[1],
                               blast_position[2], blast_energy);
      l2_e.ProjectCoefficient(e_coeff);
   }
   else
   {
      FunctionCoefficient e_coeff(e0);
      l2_e.ProjectCoefficient(e_coeff);
   }
   e_gf.ProjectGridFunction(l2_e);
   e_gf = 0;
   // Sync the data location of e_gf with its base, S
   e_gf.SyncAliasMemory(S);

   // Piecewise constant elastic stiffness over the Lagrangian mesh.
   // Lambda and Mu is Lame's first and second constants
   Vector lambda(pmesh->attributes.Max());
   Vector mu(pmesh->attributes.Max());
   // lambda = param.mat.lambda;
   // mu = param.mat.mu;
   if(lambda_vec.size() == 1) {lambda =lambda_vec[0];}
   else if(lambda_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of lambda are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {lambda[i] = lambda_vec[i];}}

   if(mu_vec.size() == 1) {mu =mu_vec[0];}
   else if(mu_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of mu are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {mu[i] = mu_vec[i];}}

   PWConstCoefficient lambda_func(lambda);
   PWConstCoefficient mu_func(mu);
   
   // Project PWConstCoefficient to grid function
   L2_FECollection lambda_fec(param.mesh.order_e, pmesh->Dimension());
   ParFiniteElementSpace lambda_fes(pmesh, &lambda_fec);
   ParGridFunction lambda0_gf(&lambda_fes);
   lambda0_gf.ProjectCoefficient(lambda_func);
   
   L2_FECollection mu_fec(param.mesh.order_e, pmesh->Dimension());
   ParFiniteElementSpace mu_fes(pmesh, &mu_fec);
   ParGridFunction mu0_gf(&mu_fes);
   mu0_gf.ProjectCoefficient(mu_func);

   // Piecewise constant for material index
   Vector mat(pmesh->attributes.Max());
   for (int i = 0; i < pmesh->attributes.Max(); i++)
   {
      mat[i] = i;
   }
   PWConstCoefficient mat_func(mat);

   // Project PWConstCoefficient to grid function
   L2_FECollection mat_fec(param.mesh.order_e, pmesh->Dimension());
   ParFiniteElementSpace mat_fes(pmesh, &mat_fec);
   ParGridFunction mat_gf(&mat_fes);
   mat_gf.ProjectCoefficient(mat_func);

   // Material properties of Plasticity
   Vector tension_cutoff(pmesh->attributes.Max());
   Vector cohesion0(pmesh->attributes.Max());
   Vector cohesion1(pmesh->attributes.Max());
   Vector friction_angle(pmesh->attributes.Max());
   Vector dilation_angle(pmesh->attributes.Max());
   Vector plastic_viscosity(pmesh->attributes.Max());
   Vector pls0(pmesh->attributes.Max());
   Vector pls1(pmesh->attributes.Max());
   // tension_cutoff = param.mat.tension_cutoff;
   // cohesion0 = param.mat.cohesion0;
   // cohesion1 = param.mat.cohesion1;
   // friction_angle = param.mat.friction_angle;
   // dilation_angle = param.mat.dilation_angle;
   // pls0 = param.mat.pls0;
   // pls1 = param.mat.pls1;

   if(tension_cutoff_vec.size() == 1) {tension_cutoff =tension_cutoff_vec[0];}
   else if(tension_cutoff_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of tension_cutoff are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {tension_cutoff[i] = tension_cutoff_vec[i];}}

   if(cohesion0_vec.size() == 1) {cohesion0 =cohesion0_vec[0];}
   else if(cohesion0_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of cohesion0 are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {cohesion0[i] = cohesion0_vec[i];}}

   if(cohesion1_vec.size() == 1) {cohesion1 =cohesion1_vec[0];}
   else if(cohesion1_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of cohesion1 are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {cohesion1[i] = cohesion1_vec[i];}}

   if(friction_angle_vec.size() == 1) {friction_angle =friction_angle_vec[0];}
   else if(friction_angle_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of friction_angle are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {friction_angle[i] = friction_angle_vec[i];}}

   if(dilation_angle_vec.size() == 1) {dilation_angle =dilation_angle_vec[0];}
   else if(dilation_angle_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of dilation_angle are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {dilation_angle[i] = dilation_angle_vec[i];}}

   if(pls0_vec.size() == 1) {pls0 =pls0_vec[0];}
   else if(pls0_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of pls0 are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {pls0[i] = pls0_vec[i];}}

   if(pls1_vec.size() == 1) {pls1 =pls1_vec[0];}
   else if(pls1_vec.size() != pmesh->attributes.Max())
   {
      if (myid == 0){cout << "The number of pls1 are not consistent with material ID in the given mesh." << endl; }        
      delete pmesh;
      MPI_Finalize();
      return 3;
   }
   else {for (int i = 0; i < pmesh->attributes.Max(); i++) {pls1[i] = pls1_vec[i];}}

   if(param.mat.viscoplastic)
   {
      if(plastic_viscosity_vec.size() == 1) {plastic_viscosity =plastic_viscosity_vec[0];}
      else if(plastic_viscosity_vec.size() != pmesh->attributes.Max())
      {
         if (myid == 0){cout << "The number of plastic_viscosity are not consistent with material ID in the given mesh." << endl; }        
         delete pmesh;
         MPI_Finalize();
         return 3;
      }
      else {for (int i = 0; i < pmesh->attributes.Max(); i++) {plastic_viscosity[i] = plastic_viscosity_vec[i];}}
   }
   else
   {
      if (myid == 0){cout << "viscoplasticity is not activate." << endl; }        
      plastic_viscosity = 1.0e+300;
   }
   
   // lithostatic pressure
   s_gf=0.0;

   if(param.control.lithostatic)
   {
      LithostaticCoefficient Lithostatic_coeff(dim, xyz_gf_l2, rho0_gf, param.control.gravity, param.control.thickness);
      // LithostaticCoefficient Lithostatic_coeff(dim, y_gf_l2, z_gf_l2, rho0_gf, param.control.gravity, param.control.thickness);
      s_gf.ProjectCoefficient(Lithostatic_coeff);
   }   

   s_gf.SyncAliasMemory(S);

   // for(int i = 0; i < s_gf.Size()/3; i++)
   // {
   //    cout << "sxx " << s_gf[i+0*s_gf.Size()/3] << ", syy " << s_gf[i+1*s_gf.Size()/3] << ", sxy " << s_gf[i+2*s_gf.Size()/3] << endl;;
   // }

   ParGridFunction s_old_gf(&L2FESpace_stress);
   s_old_gf=0.0;

   // Initial geometry
   x_ini_gf = x_gf; 
   x_ini_gf.SyncAliasMemory(S);
   ParGridFunction x_old_gf(&H1FESpace);
   x_old_gf = 0.0;

   // Plastic strain (J2 strain invariant)
   ParGridFunction p_gf(&L2FESpace);
   ParGridFunction p_gf_old(&L2FESpace);
   p_gf=0.0; p_gf_old = 0.0;
   Vector weak_location(dim);
   if(dim = 2){weak_location[0] = param.mat.weak_x; weak_location[1] = param.mat.weak_y;}
   else if(dim =3){weak_location[0] = param.mat.weak_x; weak_location[1] = param.mat.weak_y; weak_location[2] = param.mat.weak_z;}
   PlasticCoefficient p_coeff(dim, xyz_gf_l2, weak_location, param.mat.weak_rad, param.mat.ini_pls);
   // PlasticCoefficient p_coeff(dim, x_gf_l2, y_gf_l2, z_gf_l2, weak_location, param.mat.weak_rad, param.mat.ini_pls);
   
   p_gf.ProjectCoefficient(p_coeff);
   p_gf_old = p_gf;
   // Non-initial plastic strain
   ParGridFunction ini_p_gf(&L2FESpace);
   ParGridFunction ini_p_old_gf(&L2FESpace);
   ParGridFunction n_p_gf(&L2FESpace);
   ini_p_gf=p_gf; ini_p_old_gf=p_gf;
   n_p_gf=0.0;

   
   ParGridFunction u_gf(&H1FESpace);  // Displacment
   u_gf = 0.0;

   
   // Flattening node
   ParLinearForm flattening(&H1FESpace); 
   Array<int> nbc_bdr(pmesh->bdr_attributes.Max());   
   nbc_bdr = 0; nbc_bdr[2] = 1; // bottome boundary
   VectorArrayCoefficient bottom_node(dim);
   for (int i = 0; i < dim-1; i++)
   {
     bottom_node.Set(i, new ConstantCoefficient(0.0));
   }

   Vector bottom_node_id(pmesh->bdr_attributes.Max());
   bottom_node_id = 0.0;
   bottom_node_id(2) = 1.0;
   bottom_node.Set(dim-1, new PWConstCoefficient(bottom_node_id));
   flattening.AddBoundaryIntegrator(new VectorBoundaryLFIntegrator(bottom_node), nbc_bdr);
   flattening.Assemble();

   // L2_FECollection mat_fec(0, pmesh->Dimension());
   // ParFiniteElementSpace mat_fes(pmesh, &mat_fec);
   // ParGridFunction mat_gf(&mat_fes);
   // FunctionCoefficient mat_coeff(gamma_func);
   // mat_gf.ProjectCoefficient(mat_coeff);
   
   int source = 0; bool visc = false, vorticity = false;
   // switch (param.sim.problem)
   // {
   //    case 0: if (pmesh->Dimension() == 2) { source = 1; } visc = false; break;
   //    case 1: visc = true; break;
   //    case 2: visc = true; break;
   //    case 3: visc = true; S.HostRead(); break;
   //    case 4: visc = false; break;
   //    case 5: visc = true; break;
   //    case 6: visc = true; break;
   //    case 7: source = 2; visc = true; vorticity = true;  break;
   //    default: MFEM_ABORT("Wrong problem specification!");
   // }
   if (param.solver.impose_visc) { visc = true; }

   // std::cout << "calling LagrangianGeoOperator" << endl;
   geodynamics::LagrangianGeoOperator geo(S.Size(),
                                          H1FESpace, L2FESpace, L2FESpace_stress, ess_tdofs,
                                          rho0_coeff, scale_rho0_coeff, rho0_gf,
                                          mat_gf, source, param.solver.cfl,
                                          visc, vorticity, param.solver.p_assembly,
                                          param.solver.cg_tol, param.solver.cg_max_iter, 
                                          param.solver.ftz_tol,
                                          param.mesh.order_q, lambda0_gf, mu0_gf, param.control.mscale, param.control.gravity, param.control.thickness,
                                          param.control.winkler_foundation, param.control.winkler_rho, param.control.dyn_damping, param.control.dyn_factor, bc_id_pa);

   socketstream vis_rho, vis_v, vis_e;
   char vishost[] = "localhost";
   int  visport   = 19916;

   ParGridFunction rho_gf;
   // if (param.sim.visualization || param.sim.visit || param.sim.paraview) { geo.ComputeDensity(rho_gf); }
   // if (mass_bal) { geo.ComputeDensity(rho_gf); }
   geo.ComputeDensity(rho_gf);
   const double energy_init = geo.InternalEnergy(e_gf) +
                              geo.KineticEnergy(v_gf);

   if (param.sim.visualization)
   {
      // Make sure all MPI ranks have sent their 'v' solution before initiating
      // another set of GLVis connections (one from each rank):
      MPI_Barrier(pmesh->GetComm());
      vis_rho.precision(8);
      vis_v.precision(8);
      vis_e.precision(8);
      int Wx = 0, Wy = 0; // window position
      const int Ww = 350, Wh = 350; // window size
      int offx = Ww+10; // window offsets
      if (param.sim.problem != 0 && param.sim.problem != 4)
      {
         geodynamics::VisualizeField(vis_rho, vishost, visport, rho_gf,
                                       "Density", Wx, Wy, Ww, Wh);
      }
      Wx += offx;
      geodynamics::VisualizeField(vis_v, vishost, visport, v_gf,
                                    "Velocity", Wx, Wy, Ww, Wh);
      Wx += offx;
      geodynamics::VisualizeField(vis_e, vishost, visport, e_gf,
                                    "Specific Internal Energy", Wx, Wy, Ww, Wh);
   }

   // Save data for VisIt visualization.
   VisItDataCollection visit_dc(param.sim.basename, pmesh);
   if (param.sim.visit)
   {
      visit_dc.RegisterField("Density",  &rho_gf);
      visit_dc.RegisterField("Displacement", &u_gf);
      visit_dc.RegisterField("Velocity", &v_gf);
      visit_dc.RegisterField("Specific Internal Energy", &e_gf);
      visit_dc.RegisterField("Stress", &s_gf);
      visit_dc.RegisterField("Plastic Strain", &p_gf);
      visit_dc.RegisterField("Non-inital Plastic Strain", &n_p_gf);
      visit_dc.SetCycle(0);
      visit_dc.SetTime(0.0);
      visit_dc.Save();
   }

   ParaViewDataCollection *pd = NULL;
   if (param.sim.paraview)
   {
      pd = new ParaViewDataCollection(param.sim.basename, pmesh);
      pd->SetPrefixPath("ParaView");
      pd->RegisterField("Density",  &rho_gf);
      pd->RegisterField("Displacement", &u_gf);
      pd->RegisterField("Velocity", &v_gf);
      pd->RegisterField("Specific Internal Energy", &e_gf);
      pd->RegisterField("Stress", &s_gf);
      pd->RegisterField("Plastic Strain", &p_gf);
      pd->RegisterField("inital Plastic Strain", &ini_p_gf);
      pd->RegisterField("Non-inital Plastic Strain", &n_p_gf);
      pd->SetLevelsOfDetail(param.mesh.order_v);
      pd->SetDataFormat(VTKFormat::BINARY);
      // pd->SetDataFormat(VTKFormat::ASCII);
      pd->SetHighOrderOutput(true);
      pd->SetCycle(0);
      pd->SetTime(0.0);
      pd->Save();
   }

   // Perform time-integration (looping over the time iterations, ti, with a
   // time-step dt). The object oper is of type LagrangianGeoOperator that
   // defines the Mult() method that used by the time integrators.
   ode_solver->Init(geo);
   geo.ResetTimeStepEstimate();
   double t = 0.0, dt = 0.0, t_old, dt_old = 0.0, h_min_ini = 1.0, h_min = 1.0;
   dt = geo.GetTimeStepEstimate(S, dt); // To provide dt before the estimate, initializing is necessary
   h_min = geo.GetLengthEstimate(S, dt); // To provide dt before the estimate, initializing is necessary
   // dt = init_dt;
   bool last_step = false;
   int steps = 0;
   BlockVector S_old(S);
   long mem=0, mmax=0, msum=0;
   int checks = 0;
   //   const double internal_energy = geo.InternalEnergy(e_gf);
   //   const double kinetic_energy = geo.KineticEnergy(v_gf);
   //   if (mpi.Root())
   //   {
   //      cout << std::fixed;
   //      cout << "step " << std::setw(5) << 0
   //            << ",\tt = " << std::setw(5) << std::setprecision(4) << t
   //            << ",\tdt = " << std::setw(5) << std::setprecision(6) << dt
   //            << ",\t|IE| = " << std::setprecision(10) << std::scientific
   //            << internal_energy
   //            << ",\t|KE| = " << std::setprecision(10) << std::scientific
   //            << kinetic_energy
   //            << ",\t|E| = " << std::setprecision(10) << std::scientific
   //            << kinetic_energy+internal_energy;
   //      cout << std::fixed;
   //      if (mem_usage)
   //      {
   //         cout << ", mem: " << mmax << "/" << msum << " MB";
   //      }
   //      cout << endl;
   //   }

   if (mpi.Root()) 
   {
      std::cout<<""<<std::endl;
      std::cout<<"simulation starts"<<std::endl;
   }

   // Estimate element errors using the Zienkiewicz-Zhu error estimator.
   // Vector errors(pmesh->GetNE()); errors=0.0;
   // if(param.tmop.amr)
   // {
   //    // 11. As in Example 6p, we also need a refiner. This time the refinement
   //    //     strategy is based on a fixed threshold that is applied locally to each
   //    //     element. The global threshold is turned off by setting the total error
   //    //     fraction to zero. We also enforce a maximum refinement ratio between
   //    //     adjacent elements.
   //    ThresholdRefiner refiner(&errors);
   //    refiner.SetTotalErrorFraction(0.0); // use purely local threshold
   //    refiner.SetLocalErrorGoal(1e-4);
   //    refiner.PreferConformingRefinement();
   //    refiner.SetNCLimit(2);

   //    // 12. A derefiner selects groups of elements that can be coarsened to form
   //    //     a larger element. A conservative enough threshold needs to be set to
   //    //     prevent derefining elements that would immediately be refined again.
   //    ThresholdDerefiner derefiner(&errors);
   //    derefiner.SetThreshold(1e-4 * 0.25);
   //    derefiner.SetNCLimit(2);
   // }


   for (int ti = 1; !last_step; ti++)
   {
      if (t + dt >= param.sim.t_final)
      {
         dt = param.sim.t_final - t;
         last_step = true;
      }
      if (steps == param.sim.max_tsteps) { last_step = true; }
      S_old = S;
      t_old = t;
      p_gf_old = p_gf; ini_p_old_gf = ini_p_gf;
      geo.ResetTimeStepEstimate();
      if (ti == 50000){itime = dt;}
      // S is the vector of dofs, t is the current time, and dt is the time step
      // to advance.
      ode_solver->Step(S, t, dt);

      if(param.mat.plastic)
      {
         Returnmapping (s_gf, s_old_gf, p_gf, mat_gf, dim, lambda, mu, tension_cutoff, cohesion0, cohesion1, pls0, pls1, friction_angle, dilation_angle, plastic_viscosity, dt_old);
         n_p_gf  = ini_p_gf;
         n_p_gf -= p_gf;
         n_p_gf.Neg();
      }

      if(param.control.winkler_foundation & param.control.winkler_flat) {for( int i = 0; i < x_gf.Size(); i++ ){if(flattening[i] > 0.0){x_gf[i] = x_ini_gf[i];}}}

      if (param.tmop.tmop)
      {
         if((ti % param.tmop.remesh_steps) == 0 || (dt / itime) < param.tmop.time_reduction)
         {
            if(myid == 0)
            {
               if ((ti % param.tmop.remesh_steps) == 0){cout << "*** calling remeshing due to constant remeshing step " << param.tmop.remesh_steps << endl;}
               else if ((dt / itime) < param.tmop.time_reduction){cout << "*** calling remeshing due to time reduction of " << param.tmop.time_reduction << endl;}
            }

            ParGridFunction x_mod_gf(&H1FESpace); 

            // Store source mesh positions.
            ParMesh *pmesh_copy =  new ParMesh(*pmesh);
            ParMesh *pmesh_old  =  new ParMesh(*pmesh);
            ParMesh *pmesh_old1 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old2 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old3 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old4 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old5 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old6 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old7 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old8 =  new ParMesh(*pmesh);
            ParMesh *pmesh_old9 =  new ParMesh(*pmesh);

            x_old_gf = *pmesh->GetNodes();
            x_mod_gf = *pmesh->GetNodes();
            
            // if(param.control.winkler_foundation & param.control.winkler_flat)
            // {
            //    if(myid == 0)
            //    {
            //       cout << "*** flattening " << endl;
            //    }

            //    for( int i = 0; i < x_gf.Size(); i++ )
            //    {
            //       if(flattening[i] > 0.0){x_gf[i] = x_ini_gf[i];}
            //       // if(flattening[i] > 0.0){x_old_gf[i] = x_ini_gf[i];}
            //    }
            // }

            ti = ti-1;

            if (param.sim.visit)
            {
                  visit_dc.SetCycle(ti);
                  visit_dc.SetTime(t*0.995);
                  visit_dc.Save();
            }

            if (param.sim.paraview)
            {
                  pd->SetCycle(ti);
                  pd->SetTime(t*0.995);
                  pd->Save();
            }

            ti = ti+1;

            HR_adaptivity(pmesh_copy, x_mod_gf, ess_tdofs, myid, param.tmop.mesh_poly_deg, param.mesh.rs_levels, param.mesh.rp_levels, param.tmop.jitter, param.tmop.metric_id, param.tmop.target_id,\
                           param.tmop.lim_const, param.tmop.adapt_lim_const, param.tmop.quad_type, param.tmop.quad_order, param.tmop.solver_type, param.tmop.solver_iter, param.tmop.solver_rtol, \
                           param.tmop.solver_art_type, param.tmop.lin_solver, param.tmop.max_lin_iter, param.tmop.move_bnd, param.tmop.combomet, param.tmop.bal_expl_combo, param.tmop.hradaptivity, \
                           param.tmop.h_metric_id, param.tmop.normalization, param.tmop.verbosity_level, param.tmop.fdscheme, param.tmop.adapt_eval, param.tmop.exactaction, param.solver.p_assembly, \
                           param.tmop.n_hr_iter, param.tmop.n_h_iter, param.tmop.mesh_node_ordering, param.tmop.barrier_type, param.tmop.worst_case_type);

            mesh_changed = true;

            x_gf = x_mod_gf; x_gf *= param.tmop.ale;
            x_gf.Add(1.0 - param.tmop.ale, x_old_gf);

            // L2 target xyz after remeshing
            xyz_gf_l2.ProjectCoefficient(xyz_coeff);
            pmesh_copy->NewNodes(x_gf, false); // Deformined mesh for H1 interpolation 

            {
               ParGridFunction U(&H1FESpace); 
               U =0.0; U = x_old_gf;
               // x_gf += 0.01e3; // test for translation
               ParGridFunction S1(&L2FESpace); ParGridFunction S2(&L2FESpace); ParGridFunction S3(&L2FESpace);
               ParGridFunction S4(&L2FESpace); ParGridFunction S5(&L2FESpace); ParGridFunction S6(&L2FESpace);
               S1 =0.0; S2 =0.0; S3 =0.0; 
               S4 =0.0; S5 =0.0; S6 =0.0;

               if(dim == 2){for(int i = 0; i < S1.Size(); i++ ){S1[i] = s_gf[i+S1.Size()*0];S2[i] = s_gf[i+S1.Size()*1];S3[i] = s_gf[i+S1.Size()*2];} }
               else{for(int i = 0; i < S1.Size(); i++ ){S1[i] = s_gf[i+S1.Size()*0];S2[i] = s_gf[i+S1.Size()*1];S3[i] = s_gf[i+S1.Size()*2]; S4[i] = s_gf[i+S1.Size()*3];S5[i] = s_gf[i+S1.Size()*4];S6[i] = s_gf[i+S1.Size()*5];}}
               
               if(myid==0){std::cout << "remapping for L2" << std::endl;}
               Remapping(pmesh_old1, U, x_gf, e_gf, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
               Remapping(pmesh_old2, U, x_gf, p_gf, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
               Remapping(pmesh_old3, U, x_gf, ini_p_gf, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
               
               if(dim ==2)
               {
                  Remapping(pmesh_old4, U, x_gf, S1, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf; 
                  Remapping(pmesh_old5, U, x_gf, S2, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf; 
                  Remapping(pmesh_old6, U, x_gf, S3, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf; 
               }
               else
               {
                  Remapping(pmesh_old4, U, x_gf, S1, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
                  Remapping(pmesh_old5, U, x_gf, S2, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
                  Remapping(pmesh_old6, U, x_gf, S3, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
                  Remapping(pmesh_old7, U, x_gf, S4, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
                  Remapping(pmesh_old8, U, x_gf, S5, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
                  Remapping(pmesh_old9, U, x_gf, S6, param.mesh.order_v, param.mesh.order_e, param.solver.p_assembly,param.mesh.local_refinement); U = x_old_gf;
               }

               if(dim == 2){for(int i = 0; i < S1.Size(); i++ ){s_gf[i+S1.Size()*0]=S1[i]; s_gf[i+S1.Size()*1]=S2[i];s_gf[i+S1.Size()*2]=S3[i];} }
               else{for(int i = 0; i < S1.Size(); i++ ){s_gf[i+S1.Size()*0]=S1[i];s_gf[i+S1.Size()*1]=S2[i];s_gf[i+S1.Size()*2]=S3[i];s_gf[i+S1.Size()*3]=S4[i];s_gf[i+S1.Size()*4]=S5[i];s_gf[i+S1.Size()*5]=S6[i];}}

               if(myid==0){std::cout << "remapping for H1" << std::endl;}

               int NE_opt = pmesh_copy->GetNE(), 
                   nsp1 = L2FESpace.GetFE(0)->GetNodes().GetNPoints(), 
                   nsp2 = L2FESpace_stress.GetFE(0)->GetNodes().GetNPoints(), 
                   tar_ncomp = v_gf.VectorDim();
               // H1 space
               // Generate list of points where the grid function will be evaluated.
               Vector vxyz;
               int point_ordering;
               vxyz = *pmesh_copy->GetNodes(); // from target mesh
               point_ordering = pmesh_copy->GetNodes()->FESpace()->GetOrdering();
               int nodes_cnt = vxyz.Size() / dim;

               // Find and Interpolate FE function values on the desired points.
               Vector interp_vals(nodes_cnt*tar_ncomp);
               FindPointsGSLIB finder;
               // FindPointsGSLIB finder(MPI_COMM_WORLD);
               finder.Setup(*pmesh_old);
               finder.Interpolate(vxyz, v_gf, interp_vals, point_ordering);
               v_gf = interp_vals;

               finder.Interpolate(vxyz, u_gf, interp_vals, point_ordering);
               u_gf = interp_vals;

               // for (int i = 0; i < vxyz.Size()/2; i++)
               // {
               //    std::cout << "H1 : " << i << "/" << vxyz.Size()/2 << "= (" << vxyz[i] << "," << vxyz[i+vxyz.Size()/2] << ")" << std::endl;
               // }

               // L2 space 
               // finder.SetL2AvgType(FindPointsGSLIB::ARITHMETIC);
               
               // vxyz.SetSize(nsp1*NE*dim);
               // vxyz =0.0;
               // for (int i = 0; i < NE; i++)
               // {
               //    const FiniteElement *fe = L2FESpace.GetFE(i);
               //    const IntegrationRule ir = fe->GetNodes();
               //    ElementTransformation *et = L2FESpace.GetElementTransformation(i);

               //    DenseMatrix pos;
               //    et->Transform(ir, pos);
               //    Vector rowx(vxyz.GetData() + i*nsp1, nsp1),
               //          rowy(vxyz.GetData() + i*nsp1 + NE*nsp1, nsp1),
               //          rowz;
               //    if (dim == 3)
               //    {
               //       rowz.SetDataAndSize(vxyz.GetData() + i*nsp1 + 2*NE*nsp1, nsp1);
               //    }
               //    pos.GetRow(0, rowx);
               //    pos.GetRow(1, rowy);
               //    if (dim == 3) { pos.GetRow(2, rowz); }
               // }

               // // 
               // for (int i = 0; i < vxyz.Size()/2; i++)
               // {
               //    std::cout << "L2 : " << i << "/" << vxyz.Size()/2 << "= (" << vxyz[i] << "," << vxyz[i+vxyz.Size()/2] << ")" << std::endl;
               // }

               // point_ordering = Ordering::byNODES;
               // // nodes_cnt = vxyz.Size() / dim;
               // tar_ncomp = e_gf.VectorDim();

               // // Find and Interpolate FE function values on the desired points.
               // // interp_vals.SetSize(nodes_cnt*tar_ncomp); interp_vals = 0.0;/
               // interp_vals.SetSize(e_gf.Size()); interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, e_gf, interp_vals, point_ordering);
               // e_gf = interp_vals; interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, p_gf, interp_vals, point_ordering);
               // p_gf = interp_vals; interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, n_p_gf, interp_vals, point_ordering);
               // n_p_gf = interp_vals; interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, ini_p_gf, interp_vals, point_ordering);
               // ini_p_gf = interp_vals; interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, rho0_gf, interp_vals, point_ordering);
               // rho0_gf = interp_vals; interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, lambda0_gf, interp_vals, point_ordering);
               // lambda0_gf = interp_vals; interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, mu0_gf, interp_vals, point_ordering);
               // mu0_gf = interp_vals; interp_vals = 0.0;

               // finder.Interpolate(xyz_gf_l2, mat_gf, interp_vals, point_ordering);
               // mat_gf = interp_vals; interp_vals = 0.0;

               // // L2 space of stress
               // interp_vals.SetSize(s_gf.Size()); interp_vals = 0.0;
               // finder.Interpolate(xyz_gf_l2, s_gf, interp_vals, point_ordering);
               // s_gf = interp_vals;

               // // // L2 space of stress
               // vxyz.SetSize(nsp2*NE*dim);
               // vxyz =0.0;
               // for (int i = 0; i < NE; i++)
               // {
               //    const FiniteElement *fe = L2FESpace.GetFE(i);
               //    const IntegrationRule ir = fe->GetNodes();
               //    ElementTransformation *et = L2FESpace.GetElementTransformation(i);

               //    DenseMatrix pos;
               //    et->Transform(ir, pos);
               //    Vector rowx(vxyz.GetData() + i*nsp2, nsp2),
               //          rowy(vxyz.GetData() + i*nsp2 + NE*nsp2, nsp2),
               //          rowz;
               //    if (dim == 3)
               //    {
               //       rowz.SetDataAndSize(vxyz.GetData() + i*nsp2 + 2*NE*nsp2, nsp2);
               //    }
               //    pos.GetRow(0, rowx);
               //    pos.GetRow(1, rowy);
               //    if (dim == 3) { pos.GetRow(2, rowz); }
               // }
               // point_ordering = Ordering::byNODES;
               // nodes_cnt = vxyz.Size() / dim;
               // tar_ncomp = s_gf.VectorDim();

               // // Find and Interpolate FE function values on the desired points.
               // interp_vals.SetSize(nodes_cnt*tar_ncomp); interp_vals = 0.0;

               // std::cout << xyz_gf_l2.Size() <<","<<vxyz.Size() << std::endl;
               // std::cout << s_gf.Size() <<","<<nodes_cnt*tar_ncomp << std::endl;
               // finder.Interpolate(vxyz, s_gf, interp_vals, point_ordering);
               // s_gf = interp_vals;
               
            }
            delete pmesh_old; 
            delete pmesh_old1; 
            delete pmesh_old2;
            delete pmesh_old3; 
            delete pmesh_old4; 
            delete pmesh_old5; 
            delete pmesh_old6;
            delete pmesh_old7; 
            delete pmesh_old8; 
            delete pmesh_old9; 
            delete pmesh_copy;
            
            // if(dim ==2) {delete S1; delete S2; delete S3;}
            // else {delete S1; delete S2; delete S3; delete S4; delete S5; delete S6;}

            // if (mesh_changed & pmesh->Nonconforming())
            // {
            //    // update state and operator
            //    TMOPUpdate(S, S_old, offset, x_gf, v_gf, e_gf, s_gf, x_ini_gf, p_gf, n_p_gf, ini_p_gf, u_gf, rho0_gf, lambda0_gf, mu0_gf, mat_gf, flattening, dim, param.tmop.amr);
            //    pmesh->Rebalance();
            //    TMOPUpdate(S, S_old, offset, x_gf, v_gf, e_gf, s_gf, x_ini_gf, p_gf, n_p_gf, ini_p_gf, u_gf, rho0_gf, lambda0_gf, mu0_gf, mat_gf, flattening, dim, param.tmop.amr);
            // }

            if (mesh_changed & param.tmop.amr)
            {
               // update state and operator
               TMOPUpdate(S, S_old, offset, x_gf, v_gf, e_gf, s_gf, x_ini_gf, p_gf, n_p_gf, ini_p_gf, u_gf, rho0_gf, lambda0_gf, mu0_gf, mat_gf, flattening, dim, param.tmop.amr);
               geo.TMOPUpdate(S, true);
               pmesh->Rebalance();
               TMOPUpdate(S, S_old, offset, x_gf, v_gf, e_gf, s_gf, x_ini_gf, p_gf, n_p_gf, ini_p_gf, u_gf, rho0_gf, lambda0_gf, mu0_gf, mat_gf, flattening, dim, param.tmop.amr);
               geo.TMOPUpdate(S, false);
               ode_solver->Init(geo);
            }

            // if (param.sim.visit)
            // {
            //       visit_dc.SetCycle(ti);
            //       visit_dc.SetTime(t+t*1e-4);
            //       visit_dc.Save();
            // }

            // if (param.sim.paraview)
            // {
            //       pd->SetCycle(ti);
            //       pd->SetTime(t+t*1e-4);
            //       pd->Save();
            // }

            // ti = ti+1;

          }
      }

      steps++;
      dt_old = dt;

      // Adaptive time step control.
      // const double dt_est = geo.GetTimeStepEstimate(S, dt);
      double dt_est = geo.GetTimeStepEstimate(S, dt);
      h_min = geo.GetLengthEstimate(S, dt);

      // if(mesh_changed){mesh_changed=false; dt_est=dt_est*1e-5;}

      // const double dt_est = geo.GetTimeStepEstimate(S, dt, mpi.Root());
      // const double dt_est = geo.GetTimeStepEstimate(S);

      // if (dt < std::numeric_limits<double>::epsilon())
      // { 
      //    if (param.sim.visit)
      //    {
      //          visit_dc.SetCycle(ti);
      //          visit_dc.SetTime(t);
      //          visit_dc.Save();
      //    }

      //    if (param.sim.paraview)
      //    {
      //          pd->SetCycle(ti);
      //          pd->SetTime(t);
      //          pd->Save();
      //    }

      //    MFEM_ABORT("The time step crashed!"); 
      // }

      if(mesh_changed)
      {
      mesh_changed = false;
      }
      else
      {
         if (dt_est < dt)
         {
         // Repeat (solve again) with a decreased time step - decrease of the
         // time estimate suggests appearance of oscillations.
         dt *= 0.50;
         // if (dt < std::numeric_limits<double>::epsilon())
         if (dt < 1.0E-38)
         { 
            if (param.sim.visit)
            {
                  visit_dc.SetCycle(ti);
                  visit_dc.SetTime(t);
                  visit_dc.Save();
            }

            if (param.sim.paraview)
            {
                  pd->SetCycle(ti);
                  pd->SetTime(t);
                  pd->Save();
            }

            MFEM_ABORT("The time step crashed!"); 
         }
            t = t_old;
            S = S_old;
            p_gf = p_gf_old; ini_p_gf = ini_p_old_gf;
            geo.ResetQuadratureData();
            // if (mpi.Root()) { cout << "Repeating step " << ti << ", dt " << dt/86400/365.25 << std::setprecision(6) << std::scientific << " yr" << endl; }
            if (steps < param.sim.max_tsteps) { last_step = false; }
            ti--; continue;
         }
         else if (dt_est > 1.25 * dt) { dt *= 1.02; }
      }

      // Ensure the sub-vectors x_gf, v_gf, and e_gf know the location of the
      // data in S. This operation simply updates the Memory validity flags of
      // the sub-vectors to match those of S.

      // // Estimate element errors using the Zienkiewicz-Zhu error estimator.
      // if(param.tmop.amr)
      // {
      //    if (myid == 0) { std::cout << "Estimating Error ... " << flush; }
      //    Vector errors(pmesh->GetNE());
      //    // geo.GetErrorEstimates(e_gf, errors);
      //    if (myid == 0) { std::cout << "done." << std::endl; }

      //    double local_max_err = errors.Max();
      //    double global_max_err;
      //    MPI_Allreduce(&local_max_err, &global_max_err, 1,MPI_DOUBLE, MPI_MAX, pmesh.GetComm());

      //    // Refine the elements whose error is larger than a fraction of the
      //    // maximum element error.
      //    const double frac = 0.7;
      //    double threshold = frac * global_max_err;
      //    if (Mpi::Root()) { cout << "Refining ..." << endl; }
      //    pmesh->RefineByError(errors, threshold);

      //    // update state and operator
      //    TMOPUpdate(S, S_old, offset, x_gf, v_gf, e_gf, s_gf, x_ini_gf, p_gf, n_p_gf, ini_p_gf, u_gf, rho0_gf, lambda0_gf, mu0_gf, mat_gf, flattening, dim, param.tmop.amr);
      //    geo.TMOPUpdate(S, true);
      //    pmesh->Rebalance();
      //    TMOPUpdate(S, S_old, offset, x_gf, v_gf, e_gf, s_gf, x_ini_gf, p_gf, n_p_gf, ini_p_gf, u_gf, rho0_gf, lambda0_gf, mu0_gf, mat_gf, flattening, dim, param.tmop.amr);
      //    geo.TMOPUpdate(S, false);
      //    ode_solver->Init(geo);
      // }
      
      // // Surface diffusion
      // {
      //    for( int i = 0; i < topo.Size(); i++ ){topo[i] = x_top[i+topo.Size()];}
      //    ConstantCoefficient diffusivity(1e-6);
      //    ParLinearForm b_top(&sub_fespace1);
      //    b_top.AddDomainIntegrator(new DomainLFIntegrator(diffusivity));
      //    b_top.Assemble();
      //    ParBilinearForm a_top(&sub_fespace1);
      //    a_top.AddDomainIntegrator(new DiffusionIntegrator);
      //    a_top.Assemble();
      //    HypreParMatrix A_top;
      //    Vector B_top, X_top;
      //    a_top.FormLinearSystem(boundary_dofs, topo, b_top, A_top, X_top, B_top);
      //    CGSolver cg(MPI_COMM_WORLD);
      //    HypreSmoother prec;
      //    prec.SetType(HypreSmoother::Chebyshev);  // Chebyshev
      //    cg.SetPreconditioner(prec);
      //    cg.SetOperator(A_top);
      //    cg.SetRelTol(1e-12);
      //    cg.SetAbsTol(0.0);
      //    cg.SetMaxIter(2000);
      //    cg.SetPrintLevel(-1);
      //    cg.Mult(B_top, X_top);
      //    a_top.RecoverFEMSolution(X_top, b_top, topo);
      //    for( int i = 0; i < topo.Size(); i++ ){x_top[i+topo.Size()] = x_top[i+topo.Size()] + (x_top[i+topo.Size()]-topo[i])*dt;}
      //    ParSubMesh::Transfer(x_top, x_gf); // update adjusted nodes on top boundary 
      // }
      
      x_gf.SyncAliasMemory(S);
      v_gf.SyncAliasMemory(S);
      e_gf.SyncAliasMemory(S);
      s_gf.SyncAliasMemory(S);
      x_ini_gf.SyncAliasMemory(S);

      s_old_gf = s_gf; // storing old Caushy stress

      // Adding stress increment to total stress and storing spin rate
      // Make sure that the mesh corresponds to the new solution state. This is
      // needed, because some time integrators use different S-type vectors
      // and the oper object might have redirected the mesh positions to those.
      pmesh->NewNodes(x_gf, false);
      u_gf.Add(dt, v_gf);

      if (last_step || (ti % param.sim.vis_steps) == 0)
      {
         double lnorm = e_gf * e_gf, norm;
         MPI_Allreduce(&lnorm, &norm, 1, MPI_DOUBLE, MPI_SUM, pmesh->GetComm());
         if (param.sim.mem_usage)
         {
            mem = GetMaxRssMB();
            MPI_Reduce(&mem, &mmax, 1, MPI_LONG, MPI_MAX, 0, pmesh->GetComm());
            MPI_Reduce(&mem, &msum, 1, MPI_LONG, MPI_SUM, 0, pmesh->GetComm());
         }
         const double internal_energy = geo.InternalEnergy(e_gf);
         const double kinetic_energy = geo.KineticEnergy(v_gf);
         double local_max_vel = std::max(fabs(v_gf.Min()), v_gf.Max())*86400*365*100;
         double global_max_vel;

         MPI_Reduce(&local_max_vel, &global_max_vel, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

         // std::cout << myid << ", local_max_vel (cm/yr) = " << local_max_vel  << std::endl;

         if(param.sim.year)
         {
            if (mpi.Root())
            {
            const double sqrt_norm = sqrt(norm);

            cout << std::fixed;
            cout << "step " << std::setw(5) << ti
                 << ",\tt = " << std::setw(5) << std::setprecision(4) << t/86400/365.25
                 << ",\tdt (yr) = " << std::setw(5) << std::setprecision(6) << std::scientific << dt/86400/365.25
                 << ",\t|e| = " << std::setw(5) << std::setprecision(3) << std::scientific
                 << sqrt_norm
                 << ", max_vel (cm/yr) = " << std::setw(5) << std::setprecision(3) << std::scientific
                 << global_max_vel
                 << ", h_min (m) = " << std::setw(5) << std::setprecision(3) << std::scientific
                 << h_min;
            //  << ",\t|IE| = " << std::setprecision(10) << std::scientific
            //  << internal_energy
            //   << ",\t|KE| = " << std::setprecision(10) << std::scientific
            //  << kinetic_energy
            //   << ",\t|E| = " << std::setprecision(10) << std::scientific
            //  << kinetic_energy+internal_energy;
            cout << std::fixed;
            if (param.sim.mem_usage)
               {
                  cout << ", mem: " << mmax << "/" << msum << " MB";
               }
            cout << endl;
            }
         }
         else
         {
            if (mpi.Root())
            {
            const double sqrt_norm = sqrt(norm);

            cout << std::fixed;
            cout << "step " << std::setw(5) << ti
                 << ",\tt = " << std::setw(5) << std::setprecision(4) << t
                 << ",\tdt = " << std::setw(5) << std::setprecision(6) << dt
                 << ",\t|e| = " << std::setprecision(10) << std::scientific
                 << sqrt_norm;
               //   << ",\t|IE| = " << std::setprecision(10) << std::scientific
               //   << internal_energy
               //   << ",\t|KE| = " << std::setprecision(10) << std::scientific
               //   << kinetic_energy;
            //   << ",\t|E| = " << std::setprecision(10) << std::scientific
            //  << kinetic_energy+internal_energy;
            cout << std::fixed;
            if (param.sim.mem_usage)
               {
                  cout << ", mem: " << mmax << "/" << msum << " MB";
               }
            cout << endl;
            }
         }
         
         // Make sure all ranks have sent their 'v' solution before initiating
         // another set of GLVis connections (one from each rank):
         MPI_Barrier(pmesh->GetComm());

         // if (param.sim.visualization || param.sim.visit || param.sim.gfprint || param.sim.paraview) { geo.ComputeDensity(rho_gf); }
         if (param.control.mass_bal) { geo.ComputeDensity(rho_gf); }
         if (param.sim.visualization)
         {
            int Wx = 0, Wy = 0; // window position
            int Ww = 350, Wh = 350; // window size
            int offx = Ww+10; // window offsets
            if (param.sim.problem != 0 && param.sim.problem != 4)
            {
               geodynamics::VisualizeField(vis_rho, vishost, visport, rho_gf,
                                             "Density", Wx, Wy, Ww, Wh);
            }
            Wx += offx;
            geodynamics::VisualizeField(vis_v, vishost, visport,
                                          v_gf, "Velocity", Wx, Wy, Ww, Wh);
            Wx += offx;
            geodynamics::VisualizeField(vis_e, vishost, visport, e_gf,
                                          "Specific Internal Energy",
                                          Wx, Wy, Ww,Wh);
            Wx += offx;
         }

         if (param.sim.visit)
         {
            visit_dc.SetCycle(ti);
            visit_dc.SetTime(t);
            visit_dc.Save();
         }

         if (param.sim.paraview)
         {
            pd->SetCycle(ti);
            pd->SetTime(t);
            pd->Save();
         }

         if (param.sim.gfprint)
         {
            std::ostringstream mesh_name, rho_name, v_name, e_name;
            mesh_name << param.sim.basename << "_" << ti << "_mesh";
            rho_name  << param.sim.basename << "_" << ti << "_rho";
            v_name << param.sim.basename << "_" << ti << "_v";
            e_name << param.sim.basename << "_" << ti << "_e";

            std::ofstream mesh_ofs(mesh_name.str().c_str());
            mesh_ofs.precision(8);
            pmesh->PrintAsOne(mesh_ofs);
            mesh_ofs.close();

            std::ofstream rho_ofs(rho_name.str().c_str());
            rho_ofs.precision(8);
            rho_gf.SaveAsOne(rho_ofs);
            rho_ofs.close();

            std::ofstream v_ofs(v_name.str().c_str());
            v_ofs.precision(8);
            v_gf.SaveAsOne(v_ofs);
            v_ofs.close();

            std::ofstream e_ofs(e_name.str().c_str());
            e_ofs.precision(8);
            e_gf.SaveAsOne(e_ofs);
            e_ofs.close();
         }
      }

      // Problems checks
      if (param.sim.check)
      {
         double lnorm = e_gf * e_gf, norm;
         MPI_Allreduce(&lnorm, &norm, 1, MPI_DOUBLE, MPI_SUM, pmesh->GetComm());
         const double e_norm = sqrt(norm);
         MFEM_VERIFY(param.mesh.rs_levels==0 && param.mesh.rp_levels==0, "check: rs, rp");
         MFEM_VERIFY(param.mesh.order_v==2, "check: order_v");
         MFEM_VERIFY(param.mesh.order_e==1, "check: order_e");
         MFEM_VERIFY(param.solver.ode_solver_type==4, "check: ode_solver_type");
         MFEM_VERIFY(param.sim.t_final == 0.6, "check: t_final");
         MFEM_VERIFY(param.solver.cfl==0.5, "check: cfl");
         MFEM_VERIFY(param.mesh.mesh_file.compare("default") == 0, "check: mesh_file");
         MFEM_VERIFY(dim==2 || dim==3, "check: dimension");
         Checks(ti, e_norm, checks);
      }
   }
   MFEM_VERIFY(!param.sim.check || checks == 2, "Check error!");

   switch (param.solver.ode_solver_type)
   {
      case 2: steps *= 2; break;
      case 3: steps *= 3; break;
      case 4: steps *= 4; break;
      case 6: steps *= 6; break;
      case 7: steps *= 2;
   }

   geo.PrintTimingData(mpi.Root(), steps, param.sim.fom);

   if (param.sim.mem_usage)
   {
      mem = GetMaxRssMB();
      MPI_Reduce(&mem, &mmax, 1, MPI_LONG, MPI_MAX, 0, pmesh->GetComm());
      MPI_Reduce(&mem, &msum, 1, MPI_LONG, MPI_SUM, 0, pmesh->GetComm());
   }

   const double energy_final = geo.InternalEnergy(e_gf) +
                               geo.KineticEnergy(v_gf);
   if (mpi.Root())
   {
      cout << endl;
      cout << "Energy  diff: " << std::scientific << std::setprecision(2)
           << fabs(energy_init - energy_final) << endl;
      if (param.sim.mem_usage)
      {
         cout << "Maximum memory resident set size: "
              << mmax << "/" << msum << " MB" << endl;
      }
   }

   // Print the error.
   // For problems 0 and 4 the exact velocity is constant in time.
   if (param.sim.problem == 0 || param.sim.problem == 4)
   {
      const double error_max = v_gf.ComputeMaxError(v_coeff),
                   error_l1  = v_gf.ComputeL1Error(v_coeff),
                   error_l2  = v_gf.ComputeL2Error(v_coeff);
      if (mpi.Root())
      {
         cout << "L_inf  error: " << error_max << endl
              << "L_1    error: " << error_l1 << endl
              << "L_2    error: " << error_l2 << endl;
      }
   }

   if (param.sim.visualization)
   {
      vis_v.close();
      vis_e.close();
   }

   // Free the used memory.
   delete ode_solver;
   delete pmesh;

   return 0;
}

void TMOPUpdate(BlockVector &S, BlockVector &S_old,
               Array<int> &offset,
               ParGridFunction &x_gf,
               ParGridFunction &v_gf,
               ParGridFunction &e_gf,
               ParGridFunction &s_gf,
               ParGridFunction &x_ini_gf,
               ParGridFunction &p_gf,
               ParGridFunction &n_p_gf,
               ParGridFunction &ini_p_gf,
               ParGridFunction &u_gf,
               ParGridFunction &rho0_gf,
               ParGridFunction &lambda0_gf,
               ParGridFunction &mu0_gf,
               ParGridFunction &mat_gf,
               ParLinearForm &flattening,
               int dim, bool amr)
{
   ParFiniteElementSpace* H1FESpace = x_gf.ParFESpace();
   ParFiniteElementSpace* L2FESpace = e_gf.ParFESpace();
   ParFiniteElementSpace* L2FESpace_stress = s_gf.ParFESpace();

   H1FESpace->Update();
   L2FESpace->Update();
   L2FESpace_stress->Update();

   int Vsize_h1 = H1FESpace->GetVSize();
   int Vsize_l2 = L2FESpace->GetVSize();

   offset[0] = 0;
   offset[1] = offset[0] + Vsize_h1;
   offset[2] = offset[1] + Vsize_h1;
   offset[3] = offset[2] + Vsize_l2;
   offset[4] = offset[3] + Vsize_l2*3*(dim-1);
   offset[5] = offset[4] + Vsize_h1;

   S_old = S;
   S.Update(offset);

   x_gf.Update();
   v_gf.Update();
   e_gf.Update(); 
   s_gf.Update(); 
   x_ini_gf.Update(); 

   if(amr)
   {
      const Operator* H1Update = H1FESpace->GetUpdateOperator();
      const Operator* L2Update = L2FESpace->GetUpdateOperator();
      const Operator* L2Update_stress = L2FESpace_stress->GetUpdateOperator();

      H1Update->Mult(S_old.GetBlock(0), S.GetBlock(0));
      H1Update->Mult(S_old.GetBlock(1), S.GetBlock(1));
      L2Update->Mult(S_old.GetBlock(2), S.GetBlock(2));
      L2Update_stress->Mult(S_old.GetBlock(3), S.GetBlock(3));
      H1Update->Mult(S_old.GetBlock(4), S.GetBlock(4));
   }
   
   x_gf.MakeRef(H1FESpace, S, offset[0]);
   v_gf.MakeRef(H1FESpace, S, offset[1]);
   e_gf.MakeRef(L2FESpace, S, offset[2]);
   s_gf.MakeRef(L2FESpace_stress, S, offset[3]);
   x_ini_gf.MakeRef(H1FESpace, S, offset[4]);
   S_old.Update(offset);

   // Gridfunction update (Non-blcok vector )
   p_gf.Update();
   n_p_gf.Update();
   ini_p_gf.Update(); 
   u_gf.Update();
   rho0_gf.Update();
   lambda0_gf.Update();
   mu0_gf.Update();
   mat_gf.Update();
   
   //
   flattening.Update();
   flattening.Assemble();
   
   H1FESpace->UpdatesFinished();
   L2FESpace->UpdatesFinished();
   L2FESpace_stress->UpdatesFinished();
}

static void display_banner(std::ostream &os)
{
   os << endl
      << "       __                __               __    " << endl
      << "      / /   ____ _____ _/ /_  ____  _____/ /_   " << endl
      << "     / /   / __ `/ __ `/ __ \\/ __ \\/ ___/ __/ " << endl
      << "    / /___/ /_/ / /_/ / / / / /_/ (__  ) /_     " << endl
      << "   /_____/\\__,_/\\__, /_/ /_/\\____/____/\\__/ " << endl
      << "               /____/                           " << endl << endl;
}

static long GetMaxRssMB()
{
   struct rusage usage;
   if (getrusage(RUSAGE_SELF, &usage)) { return -1; }
#ifndef __APPLE__
   const long unit = 1024; // kilo
#else
   const long unit = 1024*1024; // mega
#endif
   return usage.ru_maxrss/unit; // mega bytes
}

static void Checks(const int ti, const double nrm, int &chk)
{
   const double eps = 1.e-13;
   //printf("\033[33m%.15e\033[m\n",nrm);

   auto check = [&](int p, int i, const double res)
   {
      auto rerr = [](const double a, const double v, const double eps)
      {
         MFEM_VERIFY(fabs(a) > eps && fabs(v) > eps, "One value is near zero!");
         const double err_a = fabs((a-v)/a);
         const double err_v = fabs((a-v)/v);
         return fmax(err_a, err_v) < eps;
      };
      if (problem == p && ti == i)
      { chk++; MFEM_VERIFY(rerr(nrm, res, eps), "P"<<problem<<", #"<<i); }
   };

   const double it_norms[2][8][2][2] = // dim, problem, {it,norm}
   {
      {
         {{5, 6.546538624534384e+00}, { 27, 7.588576357792927e+00}},
         {{5, 3.508254945225794e+00}, { 15, 2.756444596823211e+00}},
         {{5, 1.020745795651244e+01}, { 59, 1.721590205901898e+01}},
         {{5, 8.000000000000000e+00}, { 16, 8.000000000000000e+00}},
         {{5, 3.446324942352448e+01}, { 18, 3.446844033767240e+01}},
         {{5, 1.030899557252528e+01}, { 36, 1.057362418574309e+01}},
         {{5, 8.039707010835693e+00}, { 36, 8.316970976817373e+00}},
         {{5, 1.514929259650760e+01}, { 25, 1.514931278155159e+01}},
      },
      {
         {{5, 1.198510951452527e+03}, {188, 1.199384410059154e+03}},
         {{5, 1.339163718592566e+01}, { 28, 7.521073677397994e+00}},
         {{5, 2.041491591302486e+01}, { 59, 3.443180411803796e+01}},
         {{5, 1.600000000000000e+01}, { 16, 1.600000000000000e+01}},
         {{5, 6.892649884704898e+01}, { 18, 6.893688067534482e+01}},
         {{5, 2.061984481890964e+01}, { 36, 2.114519664792607e+01}},
         {{5, 1.607988713996459e+01}, { 36, 1.662736010353023e+01}},
         {{5, 3.029858112572883e+01}, { 24, 3.029858832743707e+01}}
      }
   };

   for (int p=0; p<8; p++)
   {
      for (int i=0; i<2; i++)
      {
         const int it = it_norms[dim-2][p][i][0];
         const double norm = it_norms[dim-2][p][i][1];
         check(p, it, norm);
      }
   }
}