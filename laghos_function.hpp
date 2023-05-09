#include "mfem.hpp"
namespace mfem
{
   double e0(const Vector &);
   double p0(const Vector &);
   double depth0(const Vector &);
   double rho0(const Vector &);
   double gamma_func(const Vector &);
   void v0(const Vector &, Vector &);
   double x_l2(const Vector &);
   double y_l2(const Vector &);
   double z_l2(const Vector &);

   class PlasticCoefficient : public VectorCoefficient
   {
   private:
      ParGridFunction &x, &y, &z;
      int dim;
      Vector location;
      double rad, ini_pls;

   public:
      PlasticCoefficient (int &_dim, ParGridFunction &_x, ParGridFunction &_y, ParGridFunction &_z, Vector &_location, double &_rad, double &_ini_pls)
         : VectorCoefficient(_dim), x(_x), y(_y), z(_z)  
         {
            dim=_dim; location = _location; rad = _rad; ini_pls = _ini_pls;
         }
      virtual void Eval(Vector &K, ElementTransformation &T, const IntegrationPoint &ip)
      {
         K.SetSize(1);
         double r = 0.0;
         double xc = x.GetValue(T, ip);
         double yc = y.GetValue(T, ip);
         double zc = z.GetValue(T, ip);

         if(dim == 2){ r = sqrt(pow((xc-location[0]), 2) + pow((yc-location[1]), 2));}
         else if(dim == 3){r = sqrt(pow((xc-location[0]), 2) + pow((yc-location[1]), 2) + pow((zc-location[2]), 2));}

         if(r <= rad)
         {
            K(0) = ini_pls;
         }
         else
         {
            K(0) = 0.0;
         }
      }
      virtual ~PlasticCoefficient() { }
   };

   class LithostaticCoefficient : public VectorCoefficient
   {
   private:
      ParGridFunction &y, &z, &rho;
      int dim;
      double gravity, thickness;

   public:
      LithostaticCoefficient (int &_dim, ParGridFunction &_y, ParGridFunction &_z, ParGridFunction &_rho, double &_gravity, double &_thickness)
         : VectorCoefficient(_dim), y(_y), z(_z), rho(_rho)  
         {
            dim=_dim; gravity = _gravity; thickness = _thickness; 
         }
      virtual void Eval(Vector &K, ElementTransformation &T, const IntegrationPoint &ip)
      {
         K.SetSize(3*(dim-1));

         double yc = y.GetValue(T, ip);
         double zc = z.GetValue(T, ip);
         double denc = rho.GetValue(T, ip);

         if(dim == 2)
         {
            K(0) = -1.0*fabs(thickness - yc)*denc*gravity;
            K(1) = -1.0*fabs(thickness - yc)*denc*gravity;
         }
         else if(dim ==3)
         {
            K(0) = -1.0*fabs(thickness - zc)*denc*gravity;
            K(1) = -1.0*fabs(thickness - zc)*denc*gravity;
            K(2) = -1.0*fabs(thickness - zc)*denc*gravity;
         }
      }
      virtual ~LithostaticCoefficient() { }
   };
}